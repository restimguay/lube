/*
 * nn/ops.c - what Tensors compute: each operation's forward pass (an Array
 * from Arrays, and a Node if the graph needs one) and its backward pass
 * (the gradients of its inputs from its output's). (Part of nn.c.)
 */

/* The code once for float32 (T is float) and once for float64 (T is double), run for dt. */
#define BY_DTYPE(dt, ...)                                                                                      \
    do {                                                                                                       \
        if ((dt) == ARR_F32) {                                                                                 \
            typedef float T;                                                                                   \
            __VA_ARGS__                                                                                        \
        } else {                                                                                               \
            typedef double T;                                                                                  \
            __VA_ARGS__                                                                                        \
        }                                                                                                      \
    } while (0)

#define IS_FLOAT_DT(t) ((t) == ARR_F64 || (t) == ARR_F32)

/* An operand as an operation takes it: a Tensor, an Array or a Number as it is (borrowed); a list as a float32
 * Array (in *made, to let go of). Nothing after an error (in *err). */
static LubeValue operand(LubeVM *vm, LubeValue v, LubeValue *made, LubeValue *err) {
    *made = lube->nothing();
    int k = lube->kind(v);
    if (tensor_of(v) || arr->get(v) || k == LUBE_NUMBER) return v;
    if (k == LUBE_LIST) {
        *made = arr->from(vm, v, ARR_F32);
        if (!lube->failed(*made)) return *made;
        *err = *made;
        *made = lube->nothing();
        return lube->nothing();
    }
    *err = lube->error(vm, "expected a Tensor, an Array, a Number or a list, got %s", lube->a_type(v));
    return lube->nothing();
}

/* x's data as a C-contiguous Array of a float dtype (its own float dtype, else float32). */
static LubeValue float_data(LubeVM *vm, LubeValue x) {
    Array *a = array_of(x);
    int dt = a && a->dtype == ARR_F64 ? ARR_F64 : ARR_F32;
    return arr->contiguous(vm, data_of(x), dt);
}

/* ---------- elementwise: + - * / ** and the activations ---------- */

static const int BIN_OPS[] = {ARR_ADD, ARR_SUB, ARR_MUL, ARR_DIV};

/* a op b (T_ADD, T_SUB, T_MUL, T_DIV, T_POW), broadcasting */
static LubeValue t_binary(LubeVM *vm, int op, LubeValue av, LubeValue bv) {
    LubeValue ma, mb, err;
    LubeValue a = operand(vm, av, &ma, &err);
    if (lube->kind(a) == LUBE_NOTHING) return err;
    LubeValue b = operand(vm, bv, &mb, &err);
    if (lube->kind(b) == LUBE_NOTHING) {
        lube->release(ma);
        return err;
    }
    LubeValue data = arr->binary(vm, op == T_POW ? ARR_POW : BIN_OPS[op - T_ADD], data_of(a), data_of(b));
    LubeValue in[2] = {a, b};
    LubeValue r = result(vm, data, lube->failed(data) ? NULL : node_new(op, 2, in));
    lube->release(ma);
    lube->release(mb);
    return r;
}

static LubeValue t_neg(LubeVM *vm, LubeValue a) { return result(vm, UN(NEG, data_of(a)), node_new(T_NEG, 1, &a)); }

/* ---------- the activations: forward (g NULL) or g times their slope, in pieces on several threads ---------- */

typedef struct {
    int op;
    const void *x, *o, *g;  /* input, the forward pass's output (for the slope), the gradient (NULL: forward) */
    void *d;
    double k, k2;           /* leaky_relu's slope, clamp's bounds */
} ActJob;

#define ACT_BLOCK 256  /* (GELU works in blocks, in two passes over each: few constants in each loop) */

static void act_f32(void *ctx, int64_t from, int64_t to) {
    const ActJob *j = (const ActJob *)ctx;
    const float *x = (const float *)j->x + from, *o = j->o ? (const float *)j->o + from : NULL;
    const float *g = j->g ? (const float *)j->g + from : NULL;
    float *d = (float *)j->d + from, kf = (float)j->k, k2f = (float)j->k2;
    double k = j->k;
    int64_t n = to - from;
    float bt[ACT_BLOCK], ba[ACT_BLOCK], bb[ACT_BLOCK];
    switch (j->op) {
    case T_SIGMOID:
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = vm_sigmoid(x[i]);
        else for (int64_t i = 0; i < n; i++) d[i] = g[i] * o[i] * (1 - o[i]);
        break;
    case T_RELU:
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = x[i] > 0 ? x[i] : 0;
        else for (int64_t i = 0; i < n; i++) d[i] = x[i] > 0 ? g[i] : 0;
        break;
    case T_LEAKY_RELU:
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = x[i] > 0 ? x[i] : (float)(k * x[i]);
        else for (int64_t i = 0; i < n; i++) d[i] = x[i] > 0 ? g[i] : (float)(k * g[i]);
        break;
    case T_TANH:
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = vm_tanh(x[i]);
        else for (int64_t i = 0; i < n; i++) d[i] = g[i] * (1 - o[i] * o[i]);
        break;
    case T_GELU:  /* x P(Z < x); its slope P(Z < x) + x e^(-x^2/2) / sqrt(2 pi) */
        for (int64_t s = 0; s < n; s += ACT_BLOCK) {
            int64_t m = n - s < ACT_BLOCK ? n - s : ACT_BLOCK;
            const float *xb = x + s;
            for (int64_t i = 0; i < m; i++) vm_phi_part(xb[i], &bt[i], &ba[i], &bb[i]);
            if (!g) {
                for (int64_t i = 0; i < m; i++) d[s + i] = vm_gelu_of(xb[i], vm_phi_tail(bt[i], ba[i], bb[i]));
            } else {
                for (int64_t i = 0; i < m; i++) bt[i] = vm_phi_tail(bt[i], ba[i], bb[i]);
                for (int64_t i = 0; i < m; i++) {
                    float cdf = xb[i] < 0 ? bt[i] : 1.0f - bt[i];
                    d[s + i] = g[s + i] * (cdf + xb[i] * vm_gauss(xb[i]) * 0.3989422804014327f);
                }
            }
        }
        break;
    case T_SILU:
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = x[i] / (1 + vm_exp(-x[i]));
        else
            for (int64_t i = 0; i < n; i++) {
                float sg = vm_sigmoid(x[i]);
                d[i] = g[i] * sg * (1 + x[i] * (1 - sg));
            }
        break;
    default:  /* T_CLAMP */
        if (!g) for (int64_t i = 0; i < n; i++) d[i] = x[i] < kf ? kf : x[i] > k2f ? k2f : x[i];
        else for (int64_t i = 0; i < n; i++) d[i] = x[i] >= kf && x[i] <= k2f ? g[i] : 0;
        break;
    }
}

static void act_f64(void *ctx, int64_t from, int64_t to) {
    const ActJob *j = (const ActJob *)ctx;
    const double *x = (const double *)j->x, *o = (const double *)j->o, *g = (const double *)j->g;
    double *d = (double *)j->d, k = j->k, k2 = j->k2;
    switch (j->op) {
    case T_SIGMOID:
        if (!g) for (int64_t i = from; i < to; i++) d[i] = 1 / (1 + exp(-x[i]));
        else for (int64_t i = from; i < to; i++) d[i] = g[i] * o[i] * (1 - o[i]);
        break;
    case T_RELU:
        if (!g) for (int64_t i = from; i < to; i++) d[i] = x[i] > 0 ? x[i] : 0;
        else for (int64_t i = from; i < to; i++) d[i] = x[i] > 0 ? g[i] : 0;
        break;
    case T_LEAKY_RELU:
        if (!g) for (int64_t i = from; i < to; i++) d[i] = x[i] > 0 ? x[i] : k * x[i];
        else for (int64_t i = from; i < to; i++) d[i] = x[i] > 0 ? g[i] : k * g[i];
        break;
    case T_TANH:
        if (!g) for (int64_t i = from; i < to; i++) d[i] = tanh(x[i]);
        else for (int64_t i = from; i < to; i++) d[i] = g[i] * (1 - o[i] * o[i]);
        break;
    case T_GELU:  /* (P(Z < x) as erfc's: 1 + erf(x) loses everything below -6 or so) */
        if (!g) for (int64_t i = from; i < to; i++) d[i] = 0.5 * x[i] * erfc(-x[i] * 0.7071067811865476);
        else
            for (int64_t i = from; i < to; i++)
                d[i] = g[i] * (0.5 * erfc(-x[i] * 0.7071067811865476) + x[i] * exp(-0.5 * x[i] * x[i]) * 0.3989422804014327);
        break;
    case T_SILU:
        if (!g) for (int64_t i = from; i < to; i++) d[i] = x[i] / (1 + exp(-x[i]));
        else
            for (int64_t i = from; i < to; i++) {
                double sg = 1 / (1 + exp(-x[i]));
                d[i] = g[i] * sg * (1 + x[i] * (1 - sg));
            }
        break;
    default:  /* T_CLAMP */
        if (!g) for (int64_t i = from; i < to; i++) d[i] = x[i] < k ? k : x[i] > k2 ? k2 : x[i];
        else for (int64_t i = from; i < to; i++) d[i] = x[i] >= k && x[i] <= k2 ? g[i] : 0;
        break;
    }
}

/* Runs an activation job over n elements: on several threads when there are enough. */
static void act_go(ActJob *j, int dtype, int64_t n) {
    int light = j->op == T_RELU || j->op == T_LEAKY_RELU || j->op == T_CLAMP || (j->g && j->op != T_GELU && j->op != T_SILU);
    arr->parallel(n, light ? 65536 : dtype == ARR_F32 ? 8192 : 2048, dtype == ARR_F32 ? act_f32 : act_f64, j);
}

/* an activation (or exp, log...) elementwise: k, k2 its numbers (leaky_relu's slope, clamp's bounds) */
static LubeValue t_unary(LubeVM *vm, int op, LubeValue xv, double k, double k2) {
    LubeValue mx, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    LubeValue data;
    switch (op) {
    case T_EXP: data = UN(EXP, data_of(x)); break;
    case T_LOG: data = UN(LOG, data_of(x)); break;
    case T_SQRT: data = UN(SQRT, data_of(x)); break;
    case T_ABS: data = UN(ABS, data_of(x)); break;
    default: {  /* computed here: sigmoid, tanh, relu, leaky_relu, gelu, silu, clamp */
        if (op == T_TANH && !(array_of(x) && IS_FLOAT_DT(array_of(x)->dtype))) {  /* (whole numbers: float64, as array's) */
            data = UN(TANH, data_of(x));
            break;
        }
        LubeValue src = float_data(vm, x);
        Array *s = arr->get(src);
        void *dp;
        data = arr->make_empty(vm, s->dtype, s->ndim, s->shape, &dp);
        ActJob j = {op, s->data, NULL, NULL, dp, k, k2};
        act_go(&j, s->dtype, s->size);
        lube->release(src);
        break;
    }
    }
    Node *nd = lube->failed(data) ? NULL : node_new(op, 1, &x);
    if (nd) {
        nd->k = k;
        nd->k2 = k2;
    }
    lube->release(mx);
    return result(vm, data, nd);
}

/* g times the activation's slope at x (out: its output), for the backward pass */
static LubeValue slope(LubeVM *vm, int op, LubeValue xv, LubeValue outv, LubeValue gv, double k, double k2) {
    LubeValue xs_ = float_data(vm, xv), os_ = arr->contiguous(vm, outv, arr->get(xs_)->dtype),
              gs_ = arr->contiguous(vm, gv, arr->get(xs_)->dtype);
    Array *xa = arr->get(xs_), *oa = arr->get(os_), *ga = arr->get(gs_);
    void *dp;
    LubeValue r = arr->make_empty(vm, xa->dtype, xa->ndim, xa->shape, &dp);
    ActJob j = {op, xa->data, oa->data, ga->data, dp, k, k2};
    act_go(&j, xa->dtype, xa->size);
    lube->release(xs_);
    lube->release(os_);
    lube->release(gs_);
    return r;
}

/* ---------- matmul, shapes, reductions ---------- */

static LubeValue t_matmul(LubeVM *vm, LubeValue av, LubeValue bv) {
    LubeValue ma, mb, err;
    LubeValue a = operand(vm, av, &ma, &err);
    if (lube->kind(a) == LUBE_NOTHING) return err;
    LubeValue b = operand(vm, bv, &mb, &err);
    if (lube->kind(b) == LUBE_NOTHING) {
        lube->release(ma);
        return err;
    }
    LubeValue data = arr->matmul(vm, data_of(a), data_of(b));
    if (!lube->failed(data) && !arr->get(data)) {  /* (vector @ vector: a Number, as a 0-dimensional Array) */
        LubeValue n = data;
        data = arr->from(vm, n, array_of(a)->dtype);
    }
    LubeValue in[2] = {a, b};
    LubeValue r = result(vm, data, lube->failed(data) ? NULL : node_new(T_MATMUL, 2, in));
    lube->release(ma);
    lube->release(mb);
    return r;
}

/* linear(x (..., in), weight (out, in), bias (out) or nothing): x weight^T + bias, as one operation (its
 * gradients come out in their own layouts: no transposed copies) */
static LubeValue t_linear(LubeVM *vm, LubeValue xv, LubeValue wv, LubeValue bv) {
    LubeValue mx, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    Array *xa = array_of(x), *w = array_of(wv), *b = lube->kind(bv) == LUBE_NOTHING ? NULL : array_of(bv);
    if (!xa || !w || w->ndim != 2 || xa->ndim < 1 || xa->shape[xa->ndim - 1] != w->shape[1] ||
        (b && (b->ndim != 1 || b->shape[0] != w->shape[0]))) {
        lube->release(mx);
        return lube->error(vm, "linear(): x (..., in), a weight (out, in) and a bias (out) must fit");
    }
    LubeValue wt = arr->transpose(vm, data_of(wv), NULL);
    LubeValue data = arr->matmul(vm, data_of(x), wt);
    lube->release(wt);
    if (b && !lube->failed(data)) {
        LubeValue sum = BIN(ADD, data, data_of(bv));
        lube->release(data);
        data = sum;
    }
    LubeValue in[3] = {x, wv, b ? bv : NUM(0)};
    Node *nd = lube->failed(data) ? NULL : node_new(T_LINEAR, 3, in);
    lube->release(mx);
    return result(vm, data, nd);
}

/* x reduced (T_SUM, T_MEAN, T_MAX) over one axis or ARR_ALL_AXES */
static LubeValue t_reduce(LubeVM *vm, int op, LubeValue x, int axis, int keepdims) {
    int rop = op == T_SUM ? ARR_SUM : op == T_MEAN ? ARR_MEAN : ARR_MAX;
    LubeValue data = arr->reduce(vm, rop, data_of(x), axis, keepdims);
    Node *nd = lube->failed(data) ? NULL : node_new(op, 1, &x);
    if (nd) {
        nd->axis = axis;
        nd->keepdims = keepdims;
    }
    return result(vm, data, nd);
}

static LubeValue t_reshape(LubeVM *vm, LubeValue x, int nd, const int64_t *shape) {
    LubeValue data = arr->reshape(vm, data_of(x), nd, shape);
    return result(vm, data, lube->failed(data) ? NULL : node_new(T_RESHAPE, 1, &x));
}

/* x's axes in this order (NULL: reversed) */
static LubeValue t_transpose(LubeVM *vm, LubeValue x, const int *axes) {
    Array *a = array_of(x);
    int order[ARR_MAXDIM];
    for (int d = 0; d < a->ndim; d++) order[d] = axes ? axes[d] : a->ndim - 1 - d;
    LubeValue data = arr->transpose(vm, data_of(x), order);
    Node *nd = node_new(T_TRANSPOSE, 1, &x);
    if (nd) memcpy(nd->axes, order, sizeof(int) * (size_t)a->ndim);
    return result(vm, data, nd);
}

/* x[...]: argv as Lube passes it (argv[0] is x) */
static LubeValue t_index(LubeVM *vm, int argc, const LubeValue *argv) {
    LubeValue small[8], *args = argc <= 8 ? small : (LubeValue *)malloc(sizeof(LubeValue) * (size_t)argc);
    if (!args) abort();  /* LCOV_EXCL_LINE: out of memory */
    args[0] = data_of(argv[0]);
    for (int i = 1; i < argc; i++) args[i] = data_of(argv[i]);  /* (a Tensor of positions: its Array) */
    LubeValue data = arr->index(vm, argc, args);
    if (args != small) free(args);
    if (!lube->failed(data) && !arr->get(data)) {  /* one element: as a 0-dimensional Array */
        LubeValue n = data;
        data = arr->from(vm, n, array_of(argv[0])->dtype);
        lube->release(n);
    }
    Node *nd = lube->failed(data) ? NULL : node_new(T_INDEX, argc, argv);  /* (the indexes kept, for backward) */
    return result(vm, data, nd);
}

/* An Array a slice along axis of contiguous src: positions from..from+len. */
static LubeValue slice_axis(LubeVM *vm, LubeValue srcv, int axis, int64_t from, int64_t len) {
    LubeValue c = arr->contiguous(vm, srcv, -1);
    Array *s = arr->get(c);
    int64_t shape[ARR_MAXDIM], outer = 1, inner = 1;
    for (int d = 0; d < s->ndim; d++) {
        shape[d] = d == axis ? len : s->shape[d];
        if (d < axis) outer *= s->shape[d];
        if (d > axis) inner *= s->shape[d];
    }
    void *dp;
    LubeValue r = arr->make(vm, s->dtype, s->ndim, shape, &dp);
    int z = arr->itemsize[s->dtype];
    for (int64_t o = 0; o < outer; o++)
        memcpy((char *)dp + o * len * inner * z, s->data + (o * s->shape[axis] + from) * inner * z, (size_t)(len * inner * z));
    lube->release(c);
    return r;
}

/* cat(tensors, dim) and stack(tensors, dim) */
static LubeValue t_cat(LubeVM *vm, LubeValue list, int dim_given, double dimd, int stack) {
    if (lube->kind(list) != LUBE_LIST || !lube->list_len(list)) return lube->error(vm, "%s() expects a list of Tensors, at least one", stack ? "stack" : "cat");
    int n = (int)lube->list_len(list);
    LubeValue *items = (LubeValue *)malloc(sizeof(LubeValue) * (size_t)n), *made = (LubeValue *)malloc(sizeof(LubeValue) * (size_t)n);
    if (!items || !made) abort();  /* LCOV_EXCL_LINE: out of memory */
    LubeValue r = lube->nothing(), err;
    int ok = 0;
    for (; ok < n; ok++) {
        items[ok] = operand(vm, lube->list_get(list, (size_t)ok), &made[ok], &err);
        if (lube->kind(items[ok]) == LUBE_NOTHING || lube->kind(items[ok]) == LUBE_NUMBER) {
            r = lube->kind(items[ok]) == LUBE_NUMBER ? lube->error(vm, "%s() joins Tensors, not Numbers", stack ? "stack" : "cat") : err;
            if (lube->kind(items[ok]) == LUBE_NUMBER) ok++;
            goto out;
        }
    }
    Array *first = array_of(items[0]);
    int nd = first->ndim + stack, dim = dim_given ? (int)dimd : 0;
    if (dimd != floor(dimd) || dim < -nd || dim >= nd) {
        r = lube->error(vm, "%s(): no dimension %g for Tensors of %d dimensions", stack ? "stack" : "cat", dimd, nd);
        goto out;
    }
    if (dim < 0) dim += nd;
    LubeValue *parts = (LubeValue *)malloc(sizeof(LubeValue) * (size_t)n);  /* (each as an Array, with stack's axis) */
    if (!parts) abort();  /* LCOV_EXCL_LINE: out of memory */
    LubeValue joined = lube->new_list(vm);
    for (int i = 0; i < n; i++) {
        Array *a = array_of(items[i]);
        if (stack) {
            int64_t shape[ARR_MAXDIM];
            int k = 0;
            for (int d = 0; d <= a->ndim && k < ARR_MAXDIM; d++) {
                if (d == dim) shape[k++] = 1;
                if (d < a->ndim && k < ARR_MAXDIM) shape[k++] = a->shape[d];
            }
            lube->list_push(joined, a->ndim < ARR_MAXDIM ? arr->reshape(vm, data_of(items[i]), a->ndim + 1, shape)
                                                         : lube->retain(data_of(items[i])));
        } else {
            lube->list_push(joined, lube->retain(data_of(items[i])));
        }
    }
    free(parts);
    /* joined along dim: through array's concatenate, by hand (arrays of the same rank) */
    int64_t total = 0, shape[ARR_MAXDIM];
    Array *a0 = arr->get(lube->list_get(joined, 0));
    if (a0->ndim != nd) {
        lube->release(joined);
        r = lube->error(vm, "stack(): too many dimensions");
        goto out;
    }
    memcpy(shape, a0->shape, sizeof(int64_t) * (size_t)nd);
    int dtype = a0->dtype;
    for (int i = 0; i < n; i++) {
        Array *a = arr->get(lube->list_get(joined, (size_t)i));
        int same = a->ndim == nd;
        for (int d = 0; d < nd && same; d++) same = d == dim || a->shape[d] == shape[d];
        if (!same) {
            lube->release(joined);
            r = lube->error(vm, "%s(): the Tensors' shapes do not fit together", stack ? "stack" : "cat");
            goto out;
        }
        total += a->shape[dim];
        if (a->dtype != dtype) dtype = ARR_F64;
    }
    shape[dim] = total;
    void *dp;
    LubeValue data = arr->make(vm, dtype, nd, shape, &dp);
    int64_t outer = 1, inner = 1, at = 0;
    for (int d = 0; d < nd; d++) {
        if (d < dim) outer *= shape[d];
        if (d > dim) inner *= shape[d];
    }
    int z = arr->itemsize[dtype];
    for (int i = 0; i < n; i++) {
        LubeValue c = arr->contiguous(vm, lube->list_get(joined, (size_t)i), dtype);
        Array *a = arr->get(c);
        for (int64_t o = 0; o < outer; o++)
            memcpy((char *)dp + (o * total + at) * inner * z, a->data + o * a->shape[dim] * inner * z, (size_t)(a->shape[dim] * inner * z));
        at += a->shape[dim];
        lube->release(c);
    }
    lube->release(joined);
    Node *node = node_new(T_CAT, n, items);
    if (node) {
        node->axis = dim;
        node->flag = stack;
    }
    r = result(vm, data, node);
out:
    for (int i = 0; i < ok; i++) lube->release(made[i]);
    free(items);
    free(made);
    return r;
}

/* ---------- softmax and the losses ---------- */

/* Rows of softmax (or log_softmax): each row's largest by eight at once, its exp's once each (float32's without
 * the C library) added up in double, then scaled; rows on several threads. */
typedef struct {
    const void *x;
    void *o;
    int64_t n;
    int logs;
} SoftJob;

static void softmax_rows_f32(void *ctx, int64_t from, int64_t to) {
    const SoftJob *j = (const SoftJob *)ctx;
    int64_t n = j->n;
    for (int64_t r = from; r < to; r++) {
        const float *row = (const float *)j->x + r * n;
        float *orow = (float *)j->o + r * n, mk[8];
        for (int k = 0; k < 8; k++) mk[k] = -INFINITY;
        int64_t i = 0;
        for (; i + 8 <= n; i += 8)
            for (int k = 0; k < 8; k++) mk[k] = row[i + k] > mk[k] ? row[i + k] : mk[k];
        float m = -INFINITY;
        for (int k = 0; k < 8; k++) m = mk[k] > m ? mk[k] : m;
        for (; i < n; i++) m = row[i] > m ? row[i] : m;
        double sk[8] = {0}, sum = 0;
        for (i = 0; i < n; i++) orow[i] = vm_exp(row[i] - m);  /* (alone: with the sums of doubles below in its
                                                                  loop, it would not vectorize) */
        for (i = 0; i + 8 <= n; i += 8)
            for (int k = 0; k < 8; k++) sk[k] += orow[i + k];
        for (; i < n; i++) sum += orow[i];
        sum += ((sk[0] + sk[1]) + (sk[2] + sk[3])) + ((sk[4] + sk[5]) + (sk[6] + sk[7]));
        if (j->logs) {
            double lse = m + log(sum);
            for (i = 0; i < n; i++) orow[i] = (float)(row[i] - lse);
        } else {
            double inv = 1 / sum;
            for (i = 0; i < n; i++) orow[i] = (float)(orow[i] * inv);
        }
    }
}

static void softmax_rows_f64(void *ctx, int64_t from, int64_t to) {
    const SoftJob *j = (const SoftJob *)ctx;
    int64_t n = j->n;
    for (int64_t r = from; r < to; r++) {
        const double *row = (const double *)j->x + r * n;
        double *orow = (double *)j->o + r * n, m = -INFINITY, sum = 0;
        for (int64_t i = 0; i < n; i++) m = row[i] > m ? row[i] : m;
        for (int64_t i = 0; i < n; i++) sum += orow[i] = exp(row[i] - m);
        double lse = m + log(sum);
        for (int64_t i = 0; i < n; i++) orow[i] = j->logs ? row[i] - lse : orow[i] / sum;
    }
}

/* softmax (or log_softmax) of x along axis: rows of the C-contiguous data with axis last */
static LubeValue softmax_data(LubeVM *vm, LubeValue xv, int axis, int logs) {
    Array *x = array_of(xv);
    int axes[ARR_MAXDIM], k = 0;
    for (int d = 0; d < x->ndim; d++)
        if (d != axis) axes[k++] = d;
    axes[k] = axis;
    LubeValue moved = arr->transpose(vm, data_of(xv), axes);
    LubeValue src = float_data(vm, moved);
    lube->release(moved);
    Array *s = arr->get(src);
    void *dp;
    LubeValue out = arr->make_empty(vm, s->dtype, s->ndim, s->shape, &dp);
    int64_t n = s->ndim ? s->shape[s->ndim - 1] : 1, rows = n ? s->size / n : 0;
    SoftJob j = {s->data, dp, n, logs};
    arr->parallel(rows, 16384 / (n ? n : 1) + 1, s->dtype == ARR_F32 ? softmax_rows_f32 : softmax_rows_f64, &j);
    lube->release(src);
    int back[ARR_MAXDIM];  /* (the rows' axis put back) */
    for (int d = 0; d < x->ndim; d++) back[axes[d]] = d;
    LubeValue r = arr->transpose(vm, out, back);
    lube->release(out);
    return r;
}

static int dim_of(LubeVM *vm, const char *fname, LubeValue x, double d, int *axis, LubeValue *err) {
    int nd = array_of(x)->ndim;
    if (d != floor(d) || d < -nd || d >= nd || nd == 0) {
        *err = lube->error(vm, "%s(): no dimension %g for a Tensor of %d dimensions", fname, d, nd);
        return 0;
    }
    *axis = d < 0 ? (int)d + nd : (int)d;
    return 1;
}

static LubeValue t_softmax(LubeVM *vm, LubeValue xv, double d, int logs) {
    LubeValue mx, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    int axis;
    if (!dim_of(vm, logs ? "log_softmax" : "softmax", x, d, &axis, &err)) {
        lube->release(mx);
        return err;
    }
    LubeValue data = softmax_data(vm, x, axis, logs);
    Node *nd = node_new(logs ? T_LOG_SOFTMAX : T_SOFTMAX, 1, &x);
    if (nd) nd->axis = axis;
    lube->release(mx);
    return result(vm, data, nd);
}

enum { RED_MEAN, RED_SUM, RED_NONE };

/* A loss's output from its per-element values: their mean, their sum, or themselves. */
static LubeValue reduced(LubeVM *vm, LubeValue per, int reduction) {
    if (reduction == RED_NONE) return per;
    LubeValue r = arr->reduce(vm, reduction == RED_MEAN ? ARR_MEAN : ARR_SUM, per, ARR_ALL_AXES, 0);
    lube->release(per);
    return r;
}

/* Class positions from a target (an Array, Tensor or list of whole numbers in 0..classes-1): int64, contiguous.
 * Nothing after an error. */
static LubeValue targets(LubeVM *vm, const char *fname, LubeValue tv, int64_t rows, int64_t classes, LubeValue *err) {
    LubeValue t = arr->from(vm, data_of(tv), ARR_I64);
    if (lube->failed(t)) {
        *err = t;
        return lube->nothing();
    }
    Array *a = arr->get(t);
    LubeValue c = arr->contiguous(vm, t, ARR_I64);
    lube->release(t);
    a = arr->get(c);
    int ok = a->size == rows && a->ndim <= 1;
    for (int64_t i = 0; i < a->size && ok; i++) ok = ((int64_t *)a->data)[i] >= 0 && ((int64_t *)a->data)[i] < classes;
    if (!ok) {
        lube->release(c);
        *err = lube->error(vm, "%s(): the targets must be %.0f class numbers in 0..%.0f", fname, (double)rows, (double)(classes - 1));
        return lube->nothing();
    }
    return c;
}

/* cross_entropy(logits (N, C) or (C), target (N) or a Number) and nll_loss(log_probs, target) */
static LubeValue t_class_loss(LubeVM *vm, int op, LubeValue xv, LubeValue tv, int reduction) {
    const char *fname = op == T_CROSS_ENTROPY ? "cross_entropy" : "nll_loss";
    LubeValue mx, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    Array *xa = array_of(x);
    if (!xa || xa->ndim < 1 || xa->ndim > 2) {
        lube->release(mx);
        return lube->error(vm, "%s() takes scores of shape (N, C) or (C)", fname);
    }
    int64_t rows = xa->ndim == 2 ? xa->shape[0] : 1, classes = xa->shape[xa->ndim - 1];
    LubeValue tc = targets(vm, fname, tv, rows, classes, &err);
    if (lube->kind(tc) == LUBE_NOTHING) {
        lube->release(mx);
        return err;
    }
    LubeValue logp = op == T_CROSS_ENTROPY ? softmax_data(vm, x, xa->ndim - 1, 1) : float_data(vm, x);
    LubeValue lc = arr->contiguous(vm, logp, -1);
    lube->release(logp);
    Array *la = arr->get(lc), *ta = arr->get(tc);
    void *dp;
    LubeValue per = arr->make(vm, la->dtype, xa->ndim == 2 ? 1 : 0, &rows, &dp);
    BY_DTYPE(la->dtype, {
        for (int64_t r = 0; r < rows; r++) ((T *)dp)[r] = -((const T *)la->data)[r * classes + ((int64_t *)ta->data)[r]];
    });
    LubeValue data = reduced(vm, per, reduction);
    LubeValue in[2] = {x, tc};
    Node *nd = node_new(op, 2, in);
    if (nd) {
        nd->reduction = reduction;
        if (op == T_CROSS_ENTROPY) nd->saved[0] = lube->retain(lc);  /* (log probabilities) */
    }
    lube->release(lc);
    lube->release(tc);
    lube->release(mx);
    return result(vm, data, nd);
}

/* mse_loss, l1_loss, binary_cross_entropy, binary_cross_entropy_with_logits: (input, target) */
static LubeValue t_pair_loss(LubeVM *vm, int op, LubeValue xv, LubeValue yv, int reduction) {
    LubeValue mx, my, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    LubeValue y = operand(vm, yv, &my, &err);
    if (lube->kind(y) == LUBE_NOTHING) {
        lube->release(mx);
        return err;
    }
    LubeValue xs = float_data(vm, x), ys;
    Array *xa = arr->get(xs);
    if (lube->kind(y) == LUBE_NUMBER) {  /* (a Number: the target everywhere) */
        LubeValue zero = arr->binary(vm, ARR_MUL, xs, NUM(0));
        ys = arr->binary(vm, ARR_ADD, zero, y);
        lube->release(zero);
    } else {
        ys = arr->contiguous(vm, data_of(y), xa->dtype);
    }
    Array *ya = arr->get(ys);
    LubeValue r;
    if (ya->ndim != xa->ndim || memcmp(ya->shape, xa->shape, sizeof(int64_t) * (size_t)xa->ndim)) {
        if (lube->kind(y) == LUBE_NUMBER) goto same;  /* LCOV_EXCL_LINE: (made to x's shape) */
        r = lube->error(vm, "the input and the target must have the same shape");
    } else {
    same:;
        void *dp;
        LubeValue per = arr->make(vm, xa->dtype, xa->ndim, xa->shape, &dp);
        int64_t n = xa->size;
        BY_DTYPE(xa->dtype, {
            const T *a = (const T *)xa->data, *b = (const T *)ya->data;
            T *o = (T *)dp;
            for (int64_t i = 0; i < n; i++) {
                double p = a[i], t = b[i], v;
                if (op == T_MSE) v = (p - t) * (p - t);
                else if (op == T_L1) v = fabs(p - t);
                else if (op == T_BCE) v = -(t * fmax(log(p), -100) + (1 - t) * fmax(log(1 - p), -100));
                else v = fmax(p, 0) - p * t + log1p(exp(-fabs(p)));
                o[i] = (T)v;
            }
        });
        LubeValue data = reduced(vm, per, reduction);
        LubeValue in[2] = {x, y};
        Node *nd = node_new(op, 2, in);
        if (nd) nd->reduction = reduction;
        r = result(vm, data, nd);
    }
    lube->release(xs);
    lube->release(ys);
    lube->release(mx);
    lube->release(my);
    return r;
}

/* ---------- embedding, layer_norm, dropout ---------- */

/* embedding(indices, weight (V, D)): weight's rows, shaped as indices + (D) */
static LubeValue t_embedding(LubeVM *vm, LubeValue iv, LubeValue wv) {
    Array *w = array_of(wv);
    if (!w || w->ndim != 2) return lube->error(vm, "embedding() needs a weight of shape (num, dim)");
    LubeValue err;
    LubeValue idx = arr->from(vm, data_of(iv), ARR_I64);
    if (lube->failed(idx)) return idx;
    LubeValue ic = arr->contiguous(vm, idx, ARR_I64);
    lube->release(idx);
    Array *ia = arr->get(ic);
    for (int64_t i = 0; i < ia->size; i++)
        if (((int64_t *)ia->data)[i] < 0 || ((int64_t *)ia->data)[i] >= w->shape[0]) {
            double bad = (double)((int64_t *)ia->data)[i];
            lube->release(ic);
            return lube->error(vm, "embedding(): index %.0f is not in 0..%.0f", bad, (double)(w->shape[0] - 1));
        }
    if (ia->ndim == ARR_MAXDIM) {
        lube->release(ic);
        return lube->error(vm, "embedding(): too many dimensions");
    }
    (void)err;
    LubeValue wc = arr->contiguous(vm, data_of(wv), -1);
    Array *wa = arr->get(wc);
    int64_t shape[ARR_MAXDIM], dim = wa->shape[1];
    memcpy(shape, ia->shape, sizeof(int64_t) * (size_t)ia->ndim);
    shape[ia->ndim] = dim;
    void *dp;
    LubeValue data = arr->make_empty(vm, wa->dtype, ia->ndim + 1, shape, &dp);  /* (every row is copied in) */
    int z = arr->itemsize[wa->dtype];
    for (int64_t i = 0; i < ia->size; i++)
        memcpy((char *)dp + i * dim * z, wa->data + ((int64_t *)ia->data)[i] * dim * z, (size_t)(dim * z));
    lube->release(wc);
    LubeValue in[2] = {ic, wv};
    Node *nd = node_new(T_EMBEDDING, 2, in);
    lube->release(ic);
    return result(vm, data, nd);
}

/* layer_norm(x, weight?, bias?, eps) over x's last dimension */
/* Rows of layer_norm: the mean and variance in four sums at once (in double), x-hat (if kept) and the output;
 * rows on several threads. */
typedef struct {
    const void *x, *w, *b;
    void *o, *h;      /* (h: x-hat, or NULL) */
    double *rstd;     /* (or NULL) */
    int64_t d;
    double eps;
    int f32;
} NormJob;

#define NORM_ROWS(T)                                                                                           \
    do {                                                                                                       \
        const T *ww = (const T *)j->w, *bb = (const T *)j->b;                                                  \
        for (int64_t r = from; r < to; r++) {                                                                  \
            const T *x = (const T *)j->x + r * d;                                                              \
            T *o = (T *)j->o + r * d, *h = j->h ? (T *)j->h + r * d : NULL;                                    \
            double s[4] = {0}, q[4] = {0}, mean = 0, var = 0;                                                  \
            int64_t i = 0;                                                                                     \
            for (; i + 4 <= d; i += 4)                                                                         \
                for (int k = 0; k < 4; k++) s[k] += x[i + k];                                                  \
            for (; i < d; i++) mean += x[i];                                                                   \
            mean = (mean + ((s[0] + s[1]) + (s[2] + s[3]))) / (double)d;                                       \
            for (i = 0; i + 4 <= d; i += 4)                                                                    \
                for (int k = 0; k < 4; k++) q[k] += (x[i + k] - mean) * (x[i + k] - mean);                     \
            for (; i < d; i++) var += (x[i] - mean) * (x[i] - mean);                                           \
            var += (q[0] + q[1]) + (q[2] + q[3]);                                                              \
            double rs = 1 / sqrt(var / (double)d + j->eps);                                                    \
            if (j->rstd) j->rstd[r] = rs;                                                                      \
            for (i = 0; i < d; i++) {                                                                          \
                double hv = (x[i] - mean) * rs;                                                                \
                if (h) h[i] = (T)hv;                                                                           \
                o[i] = (T)(hv * (ww ? ww[i] : 1) + (bb ? bb[i] : 0));                                          \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)

static void norm_rows(void *ctx, int64_t from, int64_t to) {
    const NormJob *j = (const NormJob *)ctx;
    int64_t d = j->d;
    if (j->f32) NORM_ROWS(float);
    else NORM_ROWS(double);
}

static LubeValue t_layer_norm(LubeVM *vm, LubeValue xv, LubeValue wv, LubeValue bv, double eps) {
    LubeValue mx, err;
    LubeValue x = operand(vm, xv, &mx, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    LubeValue xs = float_data(vm, x);
    Array *xa = arr->get(xs);
    int64_t d = xa->ndim ? xa->shape[xa->ndim - 1] : 1, rows = d ? xa->size / d : 0;
    Array *w = lube->kind(wv) == LUBE_NOTHING ? NULL : array_of(wv), *b = lube->kind(bv) == LUBE_NOTHING ? NULL : array_of(bv);
    if (!xa->ndim || (w && (w->ndim != 1 || w->shape[0] != d)) || (b && (b->ndim != 1 || b->shape[0] != d))) {
        lube->release(xs);
        lube->release(mx);
        return lube->error(vm, "layer_norm(): the weight and bias must have the last dimension's length (%.0f)", (double)d);
    }
    LubeValue wc = w ? arr->contiguous(vm, data_of(wv), xa->dtype) : lube->nothing(), bc = b ? arr->contiguous(vm, data_of(bv), xa->dtype) : lube->nothing();
    LubeValue in[3] = {x, w ? wv : NUM(0), b ? bv : NUM(0)};
    Node *nd = node_new(T_LAYER_NORM, 3, in);  /* (first: x-hat and 1/std are kept only for a backward pass) */
    void *dp, *hp = NULL, *rp = NULL;
    LubeValue data = arr->make_empty(vm, xa->dtype, xa->ndim, xa->shape, &dp);
    LubeValue xhat = nd ? arr->make_empty(vm, xa->dtype, xa->ndim, xa->shape, &hp) : lube->nothing();
    LubeValue rstd = nd ? arr->make_empty(vm, ARR_F64, 1, &rows, &rp) : lube->nothing();
    NormJob j = {xa->data, w ? arr->get(wc)->data : NULL, b ? arr->get(bc)->data : NULL, dp, hp, (double *)rp, d, eps,
                 xa->dtype == ARR_F32};
    arr->parallel(rows, 16384 / (d ? d : 1) + 1, norm_rows, &j);
    lube->release(xs);
    lube->release(wc);
    lube->release(bc);
    if (nd) {
        nd->saved[0] = xhat;
        nd->saved[1] = rstd;
        nd->flag = (w != NULL) | (b != NULL) << 1;
    }
    lube->release(mx);
    return result(vm, data, nd);
}

/* dropout(x, p, training): elements zeroed with probability p, the rest scaled by 1/(1-p) */
static LubeValue t_dropout(LubeVM *vm, LubeValue x, double p, int training) {
    if (!training || p == 0) return result(vm, lube->retain(data_of(x)), node_new(T_RESHAPE, 1, &x));
    LubeValue xs = float_data(vm, x);
    Array *xa = arr->get(xs);
    void *mp, *dp;
    LubeValue mask = arr->make_empty(vm, xa->dtype, xa->ndim, xa->shape, &mp), data = arr->make_empty(vm, xa->dtype, xa->ndim, xa->shape, &dp);
    arr->fill_random(mp, xa->dtype, xa->size, 0, 0, 1);
    BY_DTYPE(xa->dtype, {
        T *m = (T *)mp, *o = (T *)dp;
        const T *xr = (const T *)xa->data;
        for (int64_t i = 0; i < xa->size; i++) {
            m[i] = m[i] < p ? 0 : (T)(1 / (1 - p));
            o[i] = xr[i] * m[i];
        }
    });
    lube->release(xs);
    Node *nd = node_new(T_DROPOUT, 1, &x);
    if (nd) nd->saved[0] = mask;
    else lube->release(mask);
    return result(vm, data, nd);
}

/* ---------- convolution and pooling (NCHW) ---------- */

/* im2col: x (C, H, W) into cols (C*K*K, OH*OW) */
#define IM2COL(T, x, cols, C, H, W, K, S, P, OH, OW)                                                           \
    for (int64_t c = 0; c < C; c++)                                                                            \
        for (int64_t ky = 0; ky < K; ky++)                                                                     \
            for (int64_t kx = 0; kx < K; kx++) {                                                               \
                T *row = cols + ((c * K + ky) * K + kx) * OH * OW;                                             \
                for (int64_t oy = 0; oy < OH; oy++) {                                                          \
                    int64_t iy = oy * S - P + ky;                                                              \
                    for (int64_t ox = 0; ox < OW; ox++) {                                                      \
                        int64_t ix = ox * S - P + kx;                                                          \
                        row[oy * OW + ox] = iy >= 0 && iy < H && ix >= 0 && ix < W ? x[(c * H + iy) * W + ix] : 0; \
                    }                                                                                          \
                }                                                                                              \
            }

/* im2col for images from..to, on several threads */
typedef struct {
    const void *x;
    void *cols;
    int64_t C, H, W, K, S, P, OH, OW;
    int f32;
} ColJob;

static void im2col_images(void *ctx, int64_t from, int64_t to) {
    const ColJob *j = (const ColJob *)ctx;
    int64_t C = j->C, H = j->H, W = j->W, K = j->K, OH = j->OH, OW = j->OW;
    for (int64_t n = from; n < to; n++) {
        if (j->f32) {
            const float *xi = (const float *)j->x + n * C * H * W;
            float *ci = (float *)j->cols + n * C * K * K * OH * OW;
            IM2COL(float, xi, ci, C, H, W, K, j->S, j->P, OH, OW)
        } else {
            const double *xi = (const double *)j->x + n * C * H * W;
            double *ci = (double *)j->cols + n * C * K * K * OH * OW;
            IM2COL(double, xi, ci, C, H, W, K, j->S, j->P, OH, OW)
        }
    }
}

/* conv2d(x (N, C, H, W), weight (O, C, K, K), bias (O) or nothing, stride, padding) */
static LubeValue t_conv2d(LubeVM *vm, LubeValue xv, LubeValue wv, LubeValue bv, int stride, int pad) {
    Array *x = array_of(xv), *w = array_of(wv), *b = lube->kind(bv) == LUBE_NOTHING ? NULL : array_of(bv);
    if (!x || !w || x->ndim != 4 || w->ndim != 4 || w->shape[1] != x->shape[1] || w->shape[2] != w->shape[3] ||
        (b && (b->ndim != 1 || b->shape[0] != w->shape[0])) || stride < 1 || pad < 0)
        return lube->error(vm, "conv2d() takes x (N, C, H, W), a weight (O, C, K, K) and a bias (O), stride 1 or more and padding 0 or more");
    int64_t N = x->shape[0], C = x->shape[1], H = x->shape[2], W = x->shape[3], O = w->shape[0], K = w->shape[2];
    int64_t OH = (H + 2 * pad - K) / stride + 1, OW = (W + 2 * pad - K) / stride + 1;
    if (OH < 1 || OW < 1) return lube->error(vm, "conv2d(): the kernel is bigger than the padded image");
    LubeValue xs = float_data(vm, xv);
    Array *xa = arr->get(xs);
    int64_t cshape[3] = {N, C * K * K, OH * OW};
    void *cp;
    LubeValue cols = arr->make_empty(vm, xa->dtype, 3, cshape, &cp);  /* (IM2COL writes every element, 0s too) */
    ColJob cj = {xa->data, cp, C, H, W, K, stride, pad, OH, OW, xa->dtype == ARR_F32};
    arr->parallel(N, 16384 / (C * K * K * OH * OW + 1) + 1, im2col_images, &cj);
    lube->release(xs);
    int64_t w2s[2] = {O, C * K * K};
    LubeValue wc = arr->contiguous(vm, data_of(wv), xa->dtype);
    LubeValue w2 = arr->reshape(vm, wc, 2, w2s);
    lube->release(wc);
    LubeValue prod = arr->matmul(vm, w2, cols);  /* (N, O, OH*OW) */
    lube->release(w2);
    int64_t oshape[4] = {N, O, OH, OW};
    LubeValue data = arr->reshape(vm, prod, 4, oshape);
    lube->release(prod);
    if (b) {  /* (the bias added in place: the product is this function's own) */
        Array *da = arr->get(data);
        LubeValue bc = arr->contiguous(vm, data_of(bv), da->dtype);
        arr->own(da);
        int64_t plane = OH * OW;
        BY_DTYPE(da->dtype, {
            T *d = (T *)da->data;
            const T *bb = (const T *)arr->get(bc)->data;
            for (int64_t n = 0; n < N; n++)
                for (int64_t o = 0; o < O; o++) {
                    T *p = d + (n * O + o) * plane, add = bb[o];
                    for (int64_t i = 0; i < plane; i++) p[i] += add;
                }
        });
        lube->release(bc);
    }
    LubeValue in[3] = {xv, wv, b ? bv : NUM(0)};
    Node *nd = node_new(T_CONV2D, 3, in);
    if (nd) {
        nd->saved[0] = cols;
        nd->stride = stride;
        nd->pad = pad;
        nd->size = (int)K;
        nd->flag = b != NULL;
    } else {
        lube->release(cols);
    }
    return result(vm, data, nd);
}

/* max_pool2d / avg_pool2d(x (N, C, H, W), kernel, stride) */
static LubeValue t_pool(LubeVM *vm, int op, LubeValue xv, int k, int stride) {
    Array *x = array_of(xv);
    if (!x || x->ndim != 4 || k < 1 || stride < 1) return lube->error(vm, "%s() takes x (N, C, H, W), a kernel size and a stride of 1 or more", op == T_MAX_POOL ? "max_pool2d" : "avg_pool2d");
    int64_t N = x->shape[0], C = x->shape[1], H = x->shape[2], W = x->shape[3];
    int64_t OH = (H - k) / stride + 1, OW = (W - k) / stride + 1;
    if (H < k || W < k) return lube->error(vm, "pooling: the kernel is bigger than the image");
    LubeValue xs = float_data(vm, xv);
    Array *xa = arr->get(xs);
    int64_t oshape[4] = {N, C, OH, OW};
    void *dp, *ip;
    LubeValue data = arr->make(vm, xa->dtype, 4, oshape, &dp), where = arr->make(vm, ARR_I64, 4, oshape, &ip);
    BY_DTYPE(xa->dtype, {
        const T *xr = (const T *)xa->data;
        T *o = (T *)dp;
        int64_t *wi = (int64_t *)ip;
        for (int64_t p = 0; p < N * C; p++)
            for (int64_t oy = 0; oy < OH; oy++)
                for (int64_t ox = 0; ox < OW; ox++) {
                    double best = -INFINITY, sum = 0;
                    int64_t at = 0;
                    for (int64_t ky = 0; ky < k; ky++)
                        for (int64_t kx = 0; kx < k; kx++) {
                            int64_t i = (p * H + oy * stride + ky) * W + ox * stride + kx;
                            sum += xr[i];
                            if (xr[i] > best || (ky == 0 && kx == 0)) {
                                best = xr[i];
                                at = i;
                            }
                        }
                    int64_t oi = (p * OH + oy) * OW + ox;
                    o[oi] = (T)(op == T_MAX_POOL ? best : sum / (double)(k * k));
                    wi[oi] = at;
                }
    });
    lube->release(xs);
    Node *nd = node_new(op, 1, &xv);
    if (nd) {
        nd->saved[0] = where;
        nd->size = k;
        nd->stride = stride;
    } else {
        lube->release(where);
    }
    return result(vm, data, nd);
}

/* ======================================================================
 * The backward passes
 * ==================================================================== */

/* g for a loss's per-element values: spread back from its mean, its sum, or itself, to shape like a */
static LubeValue loss_grad(LubeVM *vm, LubeValue g, int reduction, const Array *per, Temps *tmp) {
    if (reduction == RED_NONE) return g;
    LubeValue s = keep(tmp, spread(vm, g, per));
    return reduction == RED_MEAN ? keep(tmp, BIN(DIV, s, NUM((double)per->size))) : s;
}

/* The gradient of transposing a matrix stack's last two axes. */
static LubeValue swap_last(LubeVM *vm, LubeValue a) {
    Array *x = arr->get(a);
    int axes[ARR_MAXDIM];
    for (int d = 0; d < x->ndim; d++) axes[d] = d;
    axes[x->ndim - 1] = x->ndim - 2;
    axes[x->ndim - 2] = x->ndim - 1;
    return arr->transpose(vm, a, axes);
}

/* An Array viewed with a new axis of length 1 at position at. */
static LubeValue with_axis(LubeVM *vm, LubeValue a, int at) {
    Array *x = arr->get(a);
    int64_t shape[ARR_MAXDIM];
    int k = 0;
    for (int d = 0; d <= x->ndim; d++) {
        if (d == at) shape[k++] = 1;
        if (d < x->ndim) shape[k++] = x->shape[d];
    }
    return arr->reshape(vm, a, x->ndim + 1, shape);
}

static void node_backward(LubeVM *vm, Node *n, LubeValue out, LubeValue g, LubeValue *grads) {
    Temps t = {{{{0}}}, 0};
    LubeValue x = n->n > 0 ? data_of(n->in[0]) : lube->nothing(), y = n->n > 1 ? data_of(n->in[1]) : lube->nothing();
    switch (n->op) {
    case T_ADD:
        grads[0] = grad_for(vm, g, n->in[0]);
        grads[1] = grad_for(vm, g, n->in[1]);
        break;
    case T_SUB:
        grads[0] = grad_for(vm, g, n->in[0]);
        if (wants(n->in[1])) grads[1] = grad_for(vm, keep(&t, UN(NEG, g)), n->in[1]);
        break;
    case T_MUL:
        if (wants(n->in[0])) grads[0] = grad_for(vm, keep(&t, BIN(MUL, g, y)), n->in[0]);
        if (wants(n->in[1])) grads[1] = grad_for(vm, keep(&t, BIN(MUL, g, x)), n->in[1]);
        break;
    case T_DIV:
        if (wants(n->in[0])) grads[0] = grad_for(vm, keep(&t, BIN(DIV, g, y)), n->in[0]);
        if (wants(n->in[1])) grads[1] = grad_for(vm, keep(&t, UN(NEG, keep(&t, BIN(DIV, keep(&t, BIN(MUL, g, out)), y)))), n->in[1]);
        break;
    case T_NEG: grads[0] = UN(NEG, g); break;
    case T_POW:  /* d(x^y)/dx = y x^(y-1); d/dy = x^y log x */
        if (wants(n->in[0])) grads[0] = grad_for(vm, keep(&t, BIN(MUL, g, keep(&t, BIN(MUL, y, keep(&t, BIN(POW, x, keep(&t, BIN(SUB, y, NUM(1))))))))), n->in[0]);
        if (wants(n->in[1])) grads[1] = grad_for(vm, keep(&t, BIN(MUL, g, keep(&t, BIN(MUL, out, keep(&t, UN(LOG, x)))))), n->in[1]);
        break;
    case T_MATMUL: {  /* (..., m, k) @ (..., k, n): dA = dC B^T, dB = A^T dC (vectors as one row or column) */
        Array *a = arr->get(x), *b = arr->get(y);
        LubeValue A = x, B = y, G = g;
        if (a->ndim == 1) {
            A = keep(&t, with_axis(vm, x, 0));
            G = keep(&t, with_axis(vm, G, arr->get(G)->ndim - (b->ndim == 1 ? 0 : 1)));
        }
        if (b->ndim == 1) {
            B = keep(&t, with_axis(vm, y, 1));
            G = keep(&t, with_axis(vm, G, arr->get(G)->ndim));
        }
        if (wants(n->in[0])) {
            LubeValue ga = keep(&t, arr->matmul(vm, G, keep(&t, swap_last(vm, B))));
            grads[0] = arr->sum_to(vm, ga, a->ndim == 1 ? 2 : a->ndim, a->ndim == 1 ? (int64_t[2]){1, a->shape[0]} : a->shape);
            if (a->ndim == 1) {
                LubeValue r = arr->reshape(vm, grads[0], 1, a->shape);
                lube->release(grads[0]);
                grads[0] = r;
            }
        }
        if (wants(n->in[1])) {
            LubeValue gb = keep(&t, arr->matmul(vm, keep(&t, swap_last(vm, A)), G));
            grads[1] = arr->sum_to(vm, gb, b->ndim == 1 ? 2 : b->ndim, b->ndim == 1 ? (int64_t[2]){b->shape[0], 1} : b->shape);
            if (b->ndim == 1) {
                LubeValue r = arr->reshape(vm, grads[1], 1, b->shape);
                lube->release(grads[1]);
                grads[1] = r;
            }
        }
    } break;
    case T_LINEAR: {  /* dx = g W, dW = g^T x (over every row), db = the sum of g's rows */
        Array *xa = arr->get(x), *w = arr->get(y);
        int64_t rows = xa->size / xa->shape[xa->ndim - 1], s2[2] = {rows, w->shape[0]}, x2s[2] = {rows, w->shape[1]};
        LubeValue g2 = keep(&t, arr->reshape(vm, keep(&t, arr->contiguous(vm, g, -1)), 2, s2));
        if (wants(n->in[0])) {
            LubeValue gx = keep(&t, arr->matmul(vm, g2, y));
            grads[0] = arr->reshape(vm, gx, xa->ndim, xa->shape);
        }
        if (wants(n->in[1])) {
            LubeValue x2 = keep(&t, arr->reshape(vm, keep(&t, arr->contiguous(vm, x, -1)), 2, x2s));
            grads[1] = arr->matmul(vm, keep(&t, arr->transpose(vm, g2, NULL)), x2);
        }
        if (wants(n->in[2])) grads[2] = arr->reduce(vm, ARR_SUM, g2, 0, 0);
    } break;
    case T_EXP: grads[0] = BIN(MUL, g, out); break;
    case T_LOG: grads[0] = BIN(DIV, g, x); break;
    case T_SQRT: grads[0] = BIN(DIV, g, keep(&t, BIN(MUL, out, NUM(2)))); break;
    case T_ABS: grads[0] = BIN(MUL, g, keep(&t, UN(SIGN, x))); break;
    case T_TANH: case T_SIGMOID: case T_RELU: case T_LEAKY_RELU: case T_GELU: case T_SILU: case T_CLAMP:
        grads[0] = slope(vm, n->op, x, out, g, n->k, n->k2);
        break;
    case T_SUM: case T_MEAN: case T_MAX: {
        Array *a = arr->get(x);
        LubeValue gg = g;
        if (n->axis != ARR_ALL_AXES && !n->keepdims) gg = keep(&t, with_axis(vm, g, n->axis));
        LubeValue full = keep(&t, spread(vm, gg, a));
        if (n->op == T_SUM) {
            grads[0] = lube->retain(full);
        } else if (n->op == T_MEAN) {
            grads[0] = BIN(DIV, full, NUM((double)(n->axis == ARR_ALL_AXES ? a->size : a->shape[n->axis])));
        } else {  /* max: shared among the elements equal to it */
            LubeValue o = n->axis != ARR_ALL_AXES && !n->keepdims ? keep(&t, with_axis(vm, out, n->axis)) : out;
            LubeValue mask = keep(&t, BIN(EQ, x, o));
            LubeValue count = keep(&t, arr->reduce(vm, ARR_SUM, mask, n->axis, 1));
            grads[0] = BIN(MUL, full, keep(&t, BIN(DIV, mask, count)));
        }
        break;
    }
    case T_RESHAPE: {
        Array *a = arr->get(x);
        LubeValue c = keep(&t, arr->contiguous(vm, g, -1));
        grads[0] = arr->reshape(vm, c, a->ndim, a->shape);
        break;
    }
    case T_TRANSPOSE: {
        Array *a = arr->get(x);
        int back[ARR_MAXDIM];
        for (int d = 0; d < a->ndim; d++) back[n->axes[d]] = d;
        grads[0] = arr->transpose(vm, g, back);
        break;
    }
    case T_INDEX: {  /* where each output element came from (the same index on 0..size-1), then added up there */
        Array *a = arr->get(x);
        int64_t size = a->size;
        void *pp;
        LubeValue pos = keep(&t, arr->make(vm, ARR_I64, a->ndim, a->shape, &pp));
        for (int64_t i = 0; i < size; i++) ((int64_t *)pp)[i] = i;
        LubeValue small[8], *args = n->n <= 8 ? small : (LubeValue *)malloc(sizeof(LubeValue) * (size_t)n->n);
        if (!args) abort();  /* LCOV_EXCL_LINE: out of memory */
        args[0] = pos;
        for (int i = 1; i < n->n; i++) args[i] = data_of(n->in[i]);
        LubeValue where = keep(&t, arr->index(vm, n->n, args));
        if (args != small) free(args);
        LubeValue wc = keep(&t, arr->from(vm, where, ARR_I64)), wcc = keep(&t, arr->contiguous(vm, wc, ARR_I64));
        LubeValue gc = keep(&t, arr->contiguous(vm, g, a->dtype == ARR_F32 ? ARR_F32 : ARR_F64));
        Array *wa = arr->get(wcc), *ga = arr->get(gc);
        void *dp;
        grads[0] = arr->make(vm, ga->dtype, a->ndim, a->shape, &dp);
        BY_DTYPE(ga->dtype, {
            for (int64_t i = 0; i < wa->size; i++) ((T *)dp)[((int64_t *)wa->data)[i]] += ((const T *)ga->data)[i];
        });
        break;
    }
    case T_CAT: {
        int64_t at = 0;
        for (int i = 0; i < n->n; i++) {
            Array *a = array_of(n->in[i]);
            int64_t len = n->flag ? 1 : a->shape[n->axis];
            if (wants(n->in[i])) {
                LubeValue part = keep(&t, slice_axis(vm, g, n->axis, at, len));
                grads[i] = arr->reshape(vm, part, a->ndim, a->shape);
            }
            at += len;
        }
        break;
    }
    case T_SOFTMAX: {  /* out (g - sum(g out)) along the axis */
        LubeValue s = keep(&t, arr->reduce(vm, ARR_SUM, keep(&t, BIN(MUL, g, out)), n->axis, 1));
        grads[0] = BIN(MUL, out, keep(&t, BIN(SUB, g, s)));
        break;
    }
    case T_LOG_SOFTMAX: {  /* g - exp(out) sum(g) */
        LubeValue s = keep(&t, arr->reduce(vm, ARR_SUM, g, n->axis, 1));
        grads[0] = BIN(SUB, g, keep(&t, BIN(MUL, keep(&t, UN(EXP, out)), s)));
        break;
    }
    case T_CROSS_ENTROPY: case T_NLL: {  /* (softmax - one hot) for cross entropy; -one hot for nll; times g */
        Array *xa = arr->get(x), *ta = arr->get(y);
        int64_t rows = xa->ndim == 2 ? xa->shape[0] : 1, classes = xa->shape[xa->ndim - 1];
        int fdt = xa->dtype == ARR_F64 ? ARR_F64 : ARR_F32;
        Array per;
        per.size = rows;
        per.ndim = xa->ndim == 2 ? 1 : 0;
        per.shape[0] = rows;
        LubeValue gr = keep(&t, arr->contiguous(vm, loss_grad(vm, g, n->reduction, &per, &t), ARR_F64));
        Array *gra = arr->get(gr);
        void *dp;
        grads[0] = arr->make(vm, fdt, xa->ndim, xa->shape, &dp);
        Array *la = n->op == T_CROSS_ENTROPY ? arr->get(n->saved[0]) : NULL;
        BY_DTYPE(fdt, {
            T *d = (T *)dp;
            for (int64_t r = 0; r < rows; r++) {
                double gw = ((const double *)gra->data)[r];
                for (int64_t c = 0; la && c < classes; c++) d[r * classes + c] = (T)(exp(((const T *)la->data)[r * classes + c]) * gw);
                d[r * classes + ((const int64_t *)ta->data)[r]] -= (T)gw;
            }
        });
        break;
    }
    case T_MSE: case T_L1: case T_BCE: case T_BCE_LOGITS: {
        LubeValue xs = keep(&t, float_data(vm, n->in[0]));
        Array *xa = arr->get(xs);
        LubeValue ys = lube->kind(y) == LUBE_NUMBER ? keep(&t, BIN(ADD, keep(&t, BIN(MUL, xs, NUM(0))), y))
                                                    : keep(&t, arr->contiguous(vm, y, xa->dtype));
        LubeValue gr = keep(&t, arr->contiguous(vm, loss_grad(vm, g, n->reduction, xa, &t), xa->dtype));
        Array *ya = arr->get(ys), *gra = arr->get(gr);
        void *dp;
        LubeValue dx = arr->make(vm, xa->dtype, xa->ndim, xa->shape, &dp);
        BY_DTYPE(xa->dtype, {
            const T *a = (const T *)xa->data, *b = (const T *)ya->data, *gg = (const T *)gra->data;
            T *d = (T *)dp;
            for (int64_t i = 0; i < xa->size; i++) {
                double p = a[i], tt = b[i], v;
                if (n->op == T_MSE) v = 2 * (p - tt);
                else if (n->op == T_L1) v = p > tt ? 1 : p < tt ? -1 : 0;
                else if (n->op == T_BCE) v = (p - tt) / fmax(p * (1 - p), 1e-12);
                else v = 1 / (1 + exp(-p)) - tt;
                d[i] = (T)(v * gg[i]);
            }
        });
        if (wants(n->in[1])) grads[1] = n->op == T_MSE || n->op == T_L1 ? UN(NEG, dx) : lube->nothing();
        if (wants(n->in[1]) && n->op != T_MSE && n->op != T_L1) {  /* d/dtarget: -log(p/(1-p)) or -logit */
            LubeValue lg = n->op == T_BCE ? keep(&t, UN(LOG, keep(&t, BIN(DIV, xs, keep(&t, BIN(SUB, NUM(1), xs)))))) : xs;
            grads[1] = BIN(MUL, keep(&t, UN(NEG, lg)), gr);
        }
        grads[0] = dx;
        break;
    }
    case T_EMBEDDING: {  /* rows added back into a zero weight */
        Array *ia = arr->get(x), *w = array_of(n->in[1]);
        int64_t dim = w->shape[1];
        LubeValue gc = keep(&t, arr->contiguous(vm, g, w->dtype == ARR_F64 ? ARR_F64 : ARR_F32));
        Array *ga = arr->get(gc);
        void *dp;
        grads[1] = arr->make(vm, ga->dtype, 2, w->shape, &dp);
        BY_DTYPE(ga->dtype, {
            for (int64_t i = 0; i < ia->size; i++) {
                T *row = (T *)dp + ((const int64_t *)ia->data)[i] * dim;
                const T *gr = (const T *)ga->data + i * dim;
                for (int64_t k = 0; k < dim; k++) row[k] += gr[k];
            }
        });
        break;
    }
    case T_LAYER_NORM: {
        Array *h = arr->get(n->saved[0]), *rs = arr->get(n->saved[1]);
        int64_t d = h->shape[h->ndim - 1], rows = d ? h->size / d : 0;
        LubeValue gc = keep(&t, arr->contiguous(vm, g, h->dtype));
        LubeValue wc = n->flag & 1 ? keep(&t, arr->contiguous(vm, data_of(n->in[1]), h->dtype)) : lube->nothing();
        Array *ga = arr->get(gc);
        void *dxp, *dwp, *dbp;
        LubeValue dx = arr->make(vm, h->dtype, h->ndim, h->shape, &dxp), dw = arr->make(vm, h->dtype, 1, &d, &dwp),
                  db = arr->make(vm, h->dtype, 1, &d, &dbp);
        BY_DTYPE(h->dtype, {
            const T *hh = (const T *)h->data, *gg = (const T *)ga->data, *ww = n->flag & 1 ? (const T *)arr->get(wc)->data : NULL;
            T *ddx = (T *)dxp, *ddw = (T *)dwp, *ddb = (T *)dbp;
            for (int64_t r = 0; r < rows; r++) {
                double s1 = 0, s2 = 0;
                for (int64_t i = 0; i < d; i++) {
                    double gh = gg[r * d + i] * (ww ? ww[i] : 1);
                    s1 += gh;
                    s2 += gh * hh[r * d + i];
                    ddw[i] += gg[r * d + i] * hh[r * d + i];
                    ddb[i] += gg[r * d + i];
                }
                double rstd = ((const double *)rs->data)[r];
                for (int64_t i = 0; i < d; i++) {
                    double gh = gg[r * d + i] * (ww ? ww[i] : 1);
                    ddx[r * d + i] = (T)(rstd * (gh - s1 / (double)d - hh[r * d + i] * s2 / (double)d));
                }
            }
        });
        grads[0] = dx;
        if (n->flag & 1) grads[1] = dw;
        else lube->release(dw);
        if (n->flag & 2) grads[2] = db;
        else lube->release(db);
        break;
    }
    case T_DROPOUT: grads[0] = BIN(MUL, g, n->saved[0]); break;
    case T_CONV2D: {
        Array *xa = arr->get(x), *w = array_of(n->in[1]), *cols = arr->get(n->saved[0]);
        int64_t N = xa->shape[0], C = xa->shape[1], H = xa->shape[2], W = xa->shape[3], O = w->shape[0], K = n->size;
        int64_t OH = arr->get(out)->shape[2], OW = arr->get(out)->shape[3];
        int64_t g3s[3] = {N, O, OH * OW}, w2s[2] = {O, C * K * K};
        LubeValue g3 = keep(&t, arr->reshape(vm, keep(&t, arr->contiguous(vm, g, cols->dtype)), 3, g3s));
        if (wants(n->in[1])) {  /* the sum over the batch of g cols^T */
            LubeValue gw3 = keep(&t, arr->matmul(vm, g3, keep(&t, swap_last(vm, n->saved[0]))));
            LubeValue gw = keep(&t, arr->reduce(vm, ARR_SUM, gw3, 0, 0));
            grads[1] = arr->reshape(vm, gw, 4, w->shape);
        }
        if (n->flag && wants(n->in[2])) {
            LubeValue s0 = keep(&t, arr->reduce(vm, ARR_SUM, g, 0, 0)), s1 = keep(&t, arr->reduce(vm, ARR_SUM, s0, 2, 0));
            grads[2] = arr->reduce(vm, ARR_SUM, s1, 1, 0);
        }
        if (wants(n->in[0])) {  /* cols' gradient W^T g, folded back into the image (col2im) */
            LubeValue wc = keep(&t, arr->contiguous(vm, data_of(n->in[1]), cols->dtype));
            LubeValue w2 = keep(&t, arr->reshape(vm, wc, 2, w2s));
            LubeValue gcols = keep(&t, arr->contiguous(vm, keep(&t, arr->matmul(vm, keep(&t, swap_last(vm, w2)), g3)), cols->dtype));
            Array *gca = arr->get(gcols);
            void *dp;
            grads[0] = arr->make(vm, cols->dtype, 4, xa->shape, &dp);
            int s = n->stride, p = n->pad;
            BY_DTYPE(cols->dtype, {
                for (int64_t b = 0; b < N; b++) {
                    const T *gc = (const T *)gca->data + b * C * K * K * OH * OW;
                    T *dx = (T *)dp + b * C * H * W;
                    for (int64_t c = 0; c < C; c++)
                        for (int64_t ky = 0; ky < K; ky++)
                            for (int64_t kx = 0; kx < K; kx++) {
                                const T *row = gc + ((c * K + ky) * K + kx) * OH * OW;
                                for (int64_t oy = 0; oy < OH; oy++) {
                                    int64_t iy = oy * s - p + ky;
                                    if (iy < 0 || iy >= H) continue;
                                    for (int64_t ox = 0; ox < OW; ox++) {
                                        int64_t ix = ox * s - p + kx;
                                        if (ix >= 0 && ix < W) dx[(c * H + iy) * W + ix] += row[oy * OW + ox];
                                    }
                                }
                            }
                }
            });
        }
        break;
    }
    default: {  /* T_MAX_POOL, T_AVG_POOL */
        Array *xa = arr->get(x), *wi = arr->get(n->saved[0]);
        int fdt = xa->dtype == ARR_F64 ? ARR_F64 : ARR_F32;
        LubeValue gc = keep(&t, arr->contiguous(vm, g, fdt));
        Array *ga = arr->get(gc);
        int64_t H = xa->shape[2], W = xa->shape[3], OH = ga->shape[2], OW = ga->shape[3], k = n->size, s = n->stride;
        void *dp;
        grads[0] = arr->make(vm, fdt, 4, xa->shape, &dp);
        BY_DTYPE(fdt, {
            const T *gg = (const T *)ga->data;
            T *d = (T *)dp;
            for (int64_t i = 0; i < ga->size; i++) {
                if (n->op == T_MAX_POOL) {
                    d[((const int64_t *)wi->data)[i]] += gg[i];
                    continue;
                }
                int64_t ox = i % OW, oy = (i / OW) % OH, pl = i / (OW * OH);
                for (int64_t ky = 0; ky < k; ky++)
                    for (int64_t kx = 0; kx < k; kx++) d[(pl * H + oy * s + ky) * W + ox * s + kx] += (T)(gg[i] / (double)(k * k));
            }
        });
        break;
    }
    }
    drop(&t);
}
