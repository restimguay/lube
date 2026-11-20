/*
 * nn/autograd.c - Tensors and the graph they make: each Tensor computed
 * from others (while gradients are on, and one of them requires them)
 * keeps a Node saying how; backward() walks the graph from the end and
 * gives every leaf that requires gradients its .grad. (Part of nn.c.)
 */

typedef struct Node Node;

typedef struct {
    LubeValue data;      /* its Array */
    LubeValue grad;      /* its gradient (an Array), or nothing */
    int requires_grad;
    int used;            /* computed by a graph backward() already went through (and let go of) */
    Node *node;          /* how it was computed (NULL: a leaf) */
    LubeValue pending;   /* (during backward: the gradient gathered so far) */
    int seen;            /* (during backward) */
} Tensor;

/* The operations a Node can record. */
enum {
    T_ADD, T_SUB, T_MUL, T_DIV, T_NEG, T_POW, T_MATMUL, T_EXP, T_LOG, T_SQRT, T_TANH, T_SIGMOID, T_RELU,
    T_LEAKY_RELU, T_GELU, T_SILU, T_ABS, T_CLAMP, T_SUM, T_MEAN, T_MAX, T_RESHAPE, T_TRANSPOSE, T_INDEX, T_CAT,
    T_SOFTMAX, T_LOG_SOFTMAX, T_CROSS_ENTROPY, T_NLL, T_MSE, T_L1, T_BCE, T_BCE_LOGITS, T_EMBEDDING,
    T_LAYER_NORM, T_DROPOUT, T_CONV2D, T_MAX_POOL, T_AVG_POOL, T_LINEAR
};

struct Node {
    int op, n;
    LubeValue *in;            /* its inputs: Tensors, Arrays or Numbers (retained) */
    LubeValue saved[3];       /* what the backward pass needs (retained Arrays), or nothing */
    double k, k2;             /* numbers: an exponent, a slope, bounds, eps, p... */
    int axis, keepdims, flag, reduction;
    int stride, pad, size;    /* (convolutions and pooling) */
    int nd;
    int64_t shape[ARR_MAXDIM];
    int axes[ARR_MAXDIM];
};

static LubeType *TensorType;

#if defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL
#endif
static THREAD_LOCAL int grad_off;  /* gradients are off in this thread (no_grad): each isolate has its own */

static Tensor *tensor_of(LubeValue v) { return (Tensor *)lube->foreign(v, TensorType); }

/* What an input holds: a Tensor's Array, or the input itself (an Array or a Number). Borrowed. */
static LubeValue data_of(LubeValue v) {
    Tensor *t = tensor_of(v);
    return t ? t->data : v;
}

static Array *array_of(LubeValue v) { return arr->get(data_of(v)); }

/* Whether an input is a Tensor that wants a gradient. */
static int wants(LubeValue v) {
    Tensor *t = tensor_of(v);
    return t && t->requires_grad;
}

static void node_free(Node *n) {
    if (!n) return;
    for (int i = 0; i < n->n; i++) lube->release(n->in[i]);
    for (int i = 0; i < 3; i++) lube->release(n->saved[i]);
    free(n->in);
    free(n);
}

static void tensor_release(void *data) {
    Tensor *t = (Tensor *)data;
    lube->release(t->data);
    lube->release(t->grad);
    lube->release(t->pending);
    node_free(t->node);
}

/* A new Tensor of data (taken), a leaf unless node (taken) says how it was computed. */
static LubeValue tensor_new(LubeVM *vm, LubeValue data, int requires_grad, Node *node) {
    void *p;
    LubeValue v = lube->new_foreign(vm, TensorType, &p);
    Tensor *t = (Tensor *)p;
    t->data = data;
    t->grad = lube->nothing();
    t->pending = lube->nothing();
    t->requires_grad = requires_grad;
    t->node = node;
    return v;
}

/* A Node for op over n inputs (retained), if gradients are on and one of them wants one; else NULL. */
static Node *node_new(int op, int n, const LubeValue *in) {
    int any = 0;
    for (int i = 0; i < n; i++) any |= wants(in[i]);
    if (!any || grad_off) return NULL;
    Node *nd = (Node *)calloc(1, sizeof(Node));
    if (!nd) abort();  /* LCOV_EXCL_LINE: out of memory */
    nd->op = op;
    nd->n = n;
    nd->in = (LubeValue *)malloc(sizeof(LubeValue) * (size_t)(n ? n : 1));
    if (!nd->in) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int i = 0; i < n; i++) nd->in[i] = lube->retain(in[i]);
    for (int i = 0; i < 3; i++) nd->saved[i] = lube->nothing();
    return nd;
}

/* The result of an operation: a Tensor of data (taken), recording node (taken) if there is one. An error in
 * data is given back as it is. */
static LubeValue result(LubeVM *vm, LubeValue data, Node *node) {
    if (lube->failed(data)) {
        node_free(node);
        return data;
    }
    if (!arr->get(data)) {  /* (Numbers in, a Number out: as a 0-dimensional Array) */
        LubeValue a = arr->from(vm, data, lube->kind(data) == LUBE_BOOL ? ARR_BOOL : ARR_F32);
        lube->release(data);
        data = a;
    }
    return tensor_new(vm, data, node != NULL, node);
}

/* ---------- values to let go of at the end of a step ---------- */

typedef struct {
    LubeValue v[48];
    int n;
} Temps;

static LubeValue keep(Temps *t, LubeValue v) {
    if (t->n < 48) t->v[t->n++] = v;
    return v;
}

static void drop(Temps *t) {
    for (int i = 0; i < t->n; i++) lube->release(t->v[i]);
    t->n = 0;
}

#define BIN(op, a, b) arr->binary(vm, ARR_##op, (a), (b))
#define UN(op, a) arr->unary(vm, ARR_##op, (a))
#define NUM(x) lube->number_value(x)

/* g summed down to the shape of input v's data (undoing broadcasting); nothing if v wants no gradient. */
static LubeValue grad_for(LubeVM *vm, LubeValue g, LubeValue v) {
    if (!wants(v)) return lube->nothing();
    Array *a = array_of(v);
    return arr->sum_to(vm, g, a->ndim, a->shape);
}

/* An Array of zeros shaped as a, of dtype (-1: a's). */
static LubeValue zeros_as(LubeVM *vm, const Array *a, int dtype) {
    return arr->make(vm, dtype < 0 ? a->dtype : dtype, a->ndim, a->shape, NULL);
}

/* g broadcast to a's shape (a copy). */
static LubeValue spread(LubeVM *vm, LubeValue g, const Array *a) {
    LubeValue z = zeros_as(vm, a, arr->get(g)->dtype), r = BIN(ADD, z, g);
    lube->release(z);
    return r;
}

/* ---------- the backward pass ---------- */

static void node_backward(LubeVM *vm, Node *n, LubeValue out, LubeValue g, LubeValue *grads);

/* Adds g (borrowed) to what Tensor t has gathered. */
static void gather(LubeVM *vm, Tensor *t, LubeValue g) {
    if (lube->kind(t->pending) == LUBE_NOTHING) {
        t->pending = lube->retain(g);
    } else {
        LubeValue sum = BIN(ADD, t->pending, g);
        lube->release(t->pending);
        t->pending = sum;
    }
}

/* backward(root, grad, retain): gradients for every leaf the root came from. grad: the root's (nothing: ones,
 * for a root of one element). */
static LubeValue backward(LubeVM *vm, LubeValue root, LubeValue grad, int retain) {
    Tensor *rt = tensor_of(root);
    if (rt->used) return lube->error(vm, "backward() through a graph that backward() went through already (use {retain_graph: true} the first time)");
    if (!rt->requires_grad) return lube->error(vm, "backward() of a Tensor that does not require gradients (nothing it came from does)");
    Array *rd = arr->get(rt->data);
    LubeValue seed;
    if (lube->kind(grad) == LUBE_NOTHING) {
        if (rd->size != 1) return lube->error(vm, "backward() of a Tensor of %.0f elements needs its gradient (backward(grad))", (double)rd->size);
        LubeValue z = zeros_as(vm, rd, -1);
        seed = BIN(ADD, z, NUM(1));
        lube->release(z);
    } else {
        Array *ga = arr->get(data_of(grad));
        if (!ga || ga->ndim != rd->ndim || memcmp(ga->shape, rd->shape, sizeof(int64_t) * (size_t)rd->ndim))
            return lube->error(vm, "backward(): the gradient must have the Tensor's shape");
        seed = arr->contiguous(vm, data_of(grad), rd->dtype);
    }
    /* the order: every Tensor after those it came from (depth-first, without recursion) */
    size_t cap = 64, count = 0, top = 0;
    LubeValue *order = (LubeValue *)malloc(sizeof(LubeValue) * cap);
    struct Frame { LubeValue v; int next; } *stack = (struct Frame *)malloc(sizeof(struct Frame) * cap);
    size_t scap = cap;
    if (!order || !stack) abort();  /* LCOV_EXCL_LINE: out of memory */
    rt->seen = 1;
    stack[top].v = root;
    stack[top++].next = 0;
    while (top) {
        struct Frame *f = &stack[top - 1];
        Tensor *t = tensor_of(f->v);
        if (t->node && f->next < t->node->n) {
            LubeValue in = t->node->in[f->next++];
            Tensor *c = tensor_of(in);
            if (c && c->used) {  /* (computed by a graph already let go of: nothing to go back through) */
                for (size_t k = 0; k < top; k++) tensor_of(stack[k].v)->seen = 0;
                for (size_t k = 0; k < count; k++) {
                    tensor_of(order[k])->seen = 0;
                    lube->release(order[k]);
                }
                free(stack);
                free(order);
                lube->release(seed);
                return lube->error(vm, "backward() through a graph that backward() went through already (use {retain_graph: true} the first time)");
            }
            if (c && c->requires_grad && !c->seen) {
                c->seen = 1;
                if (top == scap) {
                    scap *= 2;
                    stack = (struct Frame *)realloc(stack, sizeof(struct Frame) * scap);
                    if (!stack) abort();  /* LCOV_EXCL_LINE: out of memory */
                }
                stack[top].v = in;
                stack[top++].next = 0;
            }
            continue;
        }
        if (count == cap) {
            cap *= 2;
            order = (LubeValue *)realloc(order, sizeof(LubeValue) * cap);
            if (!order) abort();  /* LCOV_EXCL_LINE: out of memory */
        }
        order[count++] = lube->retain(f->v);
        top--;
    }
    free(stack);
    rt->pending = seed;
    for (size_t i = count; i-- > 0;) {
        Tensor *t = tensor_of(order[i]);
        t->seen = 0;
        LubeValue g = t->pending;
        t->pending = lube->nothing();
        if (lube->kind(g) == LUBE_NOTHING) continue;
        if (!t->node) {  /* a leaf: its .grad gathers */
            if (lube->kind(t->grad) == LUBE_NOTHING) {
                t->grad = g;
            } else {
                LubeValue sum = BIN(ADD, t->grad, g);
                lube->release(t->grad);
                lube->release(g);
                t->grad = sum;
            }
            continue;
        }
        Node *n = t->node;
        LubeValue small[4], *grads = n->n <= 4 ? small : (LubeValue *)malloc(sizeof(LubeValue) * (size_t)n->n);
        if (!grads) abort();  /* LCOV_EXCL_LINE: out of memory */
        for (int k = 0; k < n->n; k++) grads[k] = lube->nothing();
        node_backward(vm, n, t->data, g, grads);
        for (int k = 0; k < n->n; k++) {
            Tensor *c = tensor_of(n->in[k]);
            if (c && c->requires_grad && lube->kind(grads[k]) != LUBE_NOTHING) gather(vm, c, grads[k]);
            lube->release(grads[k]);
        }
        if (grads != small) free(grads);
        lube->release(g);
        if (!retain) {  /* (the graph is let go of as it is used, as PyTorch's is) */
            t->node = NULL;
            t->used = 1;
            node_free(n);
        }
    }
    for (size_t i = 0; i < count; i++) lube->release(order[i]);
    free(order);
    return lube->nothing();
}
