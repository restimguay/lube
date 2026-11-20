/*
 * nn/modules.c - layers (Linear, Conv2d, Embedding, LayerNorm...), the
 * activations and losses as modules, Sequential, and the optimizers (SGD,
 * Adam, AdamW). A module's forward(x) computes with the operations of
 * ops.c, so the graph comes with it. (Part of nn.c.)
 */

enum {
    M_LINEAR, M_CONV2D, M_EMBEDDING, M_LAYER_NORM, M_DROPOUT, M_RELU, M_SIGMOID, M_TANH, M_GELU, M_SILU,
    M_LEAKY_RELU, M_SOFTMAX, M_LOG_SOFTMAX, M_FLATTEN, M_MAX_POOL, M_AVG_POOL, M_SEQUENTIAL, M_MSE, M_L1,
    M_CROSS_ENTROPY, M_BCE, M_BCE_LOGITS, M_COUNT
};
static const char *const MODULE_NAMES[M_COUNT] = {
    "Linear", "Conv2d", "Embedding", "LayerNorm", "Dropout", "ReLU", "Sigmoid", "Tanh", "GELU", "SiLU",
    "LeakyReLU", "Softmax", "LogSoftmax", "Flatten", "MaxPool2d", "AvgPool2d", "Sequential", "MSELoss", "L1Loss",
    "CrossEntropyLoss", "BCELoss", "BCEWithLogitsLoss"};
static LubeType *ModuleTypes[M_COUNT];

typedef struct {
    int kind;
    LubeValue weight, bias;   /* Tensors (or nothing) */
    LubeValue children;       /* Sequential: its modules (a List), else nothing */
    double p;                 /* dropout's p, layer norm's eps, leaky relu's slope */
    int a, b, k, stride, pad, dim, reduction;
    int training;
} Module;

static Module *module_of(LubeValue v) {
    for (int i = 0; i < M_COUNT; i++) {
        Module *m = (Module *)lube->foreign(v, ModuleTypes[i]);
        if (m) return m;
    }
    return NULL;
}

static void module_release(void *data) {
    Module *m = (Module *)data;
    lube->release(m->weight);
    lube->release(m->bias);
    lube->release(m->children);
}

static int module_describe(const void *data, char *buf, size_t size) {
    const Module *m = (const Module *)data;
    switch (m->kind) {
    case M_LINEAR: return snprintf(buf, size, "Linear(%d, %d%s)", m->a, m->b, lube->kind(m->bias) == LUBE_NOTHING ? ", no bias" : "");
    case M_CONV2D: return snprintf(buf, size, "Conv2d(%d, %d, %d, stride %d, padding %d)", m->a, m->b, m->k, m->stride, m->pad);
    case M_EMBEDDING: return snprintf(buf, size, "Embedding(%d, %d)", m->a, m->b);
    case M_LAYER_NORM: return snprintf(buf, size, "LayerNorm(%d)", m->a);
    case M_DROPOUT: return snprintf(buf, size, "Dropout(%g)", m->p);
    case M_SEQUENTIAL: return snprintf(buf, size, "Sequential(%.0f modules)", (double)lube->list_len(m->children));
    case M_MAX_POOL: case M_AVG_POOL: return snprintf(buf, size, "%s(%d, stride %d)", MODULE_NAMES[m->kind], m->k, m->stride);
    case M_LEAKY_RELU: return snprintf(buf, size, "LeakyReLU(%g)", m->p);
    case M_SOFTMAX: case M_LOG_SOFTMAX: return snprintf(buf, size, "%s(%d)", MODULE_NAMES[m->kind], m->dim);
    default: return snprintf(buf, size, "%s()", MODULE_NAMES[m->kind]);
    }
}

/* A new module of kind, as *out. */
static LubeValue module_new(LubeVM *vm, int kind, Module **out) {
    void *p;
    LubeValue v = lube->new_foreign(vm, ModuleTypes[kind], &p);
    Module *m = (Module *)p;
    m->kind = kind;
    m->weight = lube->nothing();
    m->bias = lube->nothing();
    m->children = lube->nothing();
    m->training = 1;
    *out = m;
    return v;
}

/* A parameter: a leaf Tensor of the shape, float32, that requires gradients, uniform in [-bound, bound) (normal
 * with deviation `bound` when normal). */
static LubeValue parameter(LubeVM *vm, int nd, const int64_t *shape, double bound, int normal) {
    void *dp;
    LubeValue a = arr->make(vm, ARR_F32, nd, shape, &dp);
    int64_t n = 1;
    for (int d = 0; d < nd; d++) n *= shape[d];
    if (bound != 0) arr->fill_random(dp, ARR_F32, n, normal, normal ? 0 : -bound, bound);
    return tensor_new(vm, a, 1, NULL);
}

/* ---------- forward ---------- */

static LubeValue module_forward(LubeVM *vm, Module *m, int argc, const LubeValue *argv);

/* forward on any module: one of ours, or an object whose class has a forward method */
static LubeValue forward_any(LubeVM *vm, LubeValue mod, int argc, const LubeValue *argv) {
    Module *m = module_of(mod);
    return m ? module_forward(vm, m, argc, argv) : lube->call_method(vm, mod, "forward", argc, argv);
}

static LubeValue module_forward(LubeVM *vm, Module *m, int argc, const LubeValue *argv) {
    LubeValue x = argv[0];
    int is_loss = m->kind >= M_MSE;
    if (is_loss && argc != 2) return lube->error(vm, "%s's forward() takes the input and the target", MODULE_NAMES[m->kind]);
    if (!is_loss && argc != 1) return lube->error(vm, "%s's forward() takes one input", MODULE_NAMES[m->kind]);
    if (m->kind != M_SEQUENTIAL && m->kind != M_EMBEDDING && !is_loss && !tensor_of(x) && !arr->get(x) && lube->kind(x) != LUBE_LIST)
        return lube->error(vm, "%s's forward() expects a Tensor, an Array or a list, got %s", MODULE_NAMES[m->kind], lube->a_type(x));
    switch (m->kind) {
    case M_LINEAR: {  /* x W^T + b */
        Array *xa = array_of(x);
        if (xa && (xa->ndim == 0 || xa->shape[xa->ndim - 1] != m->a))
            return lube->error(vm, "Linear(%d, %d): the input's last dimension must be %d", m->a, m->b, m->a);
        return t_linear(vm, x, m->weight, m->bias);
    }
    case M_CONV2D: return t_conv2d(vm, x, m->weight, m->bias, m->stride, m->pad);
    case M_EMBEDDING: return t_embedding(vm, x, m->weight);
    case M_LAYER_NORM: return t_layer_norm(vm, x, m->weight, m->bias, m->p);
    case M_DROPOUT: return t_dropout(vm, x, m->p, m->training);
    case M_RELU: return t_unary(vm, T_RELU, x, 0, 0);
    case M_SIGMOID: return t_unary(vm, T_SIGMOID, x, 0, 0);
    case M_TANH: return t_unary(vm, T_TANH, x, 0, 0);
    case M_GELU: return t_unary(vm, T_GELU, x, 0, 0);
    case M_SILU: return t_unary(vm, T_SILU, x, 0, 0);
    case M_LEAKY_RELU: return t_unary(vm, T_LEAKY_RELU, x, m->p, 0);
    case M_SOFTMAX: return t_softmax(vm, x, m->dim, 0);
    case M_LOG_SOFTMAX: return t_softmax(vm, x, m->dim, 1);
    case M_FLATTEN: {  /* (N, ...) to (N, everything else) */
        Array *xa = array_of(x);
        if (!xa || xa->ndim < 1) return lube->error(vm, "Flatten's forward() needs a Tensor of at least 1 dimension");
        int64_t shape[2] = {xa->shape[0], -1};
        return t_reshape(vm, x, 2, shape);
    }
    case M_MAX_POOL: return t_pool(vm, T_MAX_POOL, x, m->k, m->stride);
    case M_AVG_POOL: return t_pool(vm, T_AVG_POOL, x, m->k, m->stride);
    case M_SEQUENTIAL: {
        LubeValue cur = lube->retain(x);
        for (size_t i = 0; i < lube->list_len(m->children); i++) {
            LubeValue next = forward_any(vm, lube->list_get(m->children, i), 1, &cur);
            lube->release(cur);
            if (lube->failed(next)) return next;
            cur = next;
        }
        return cur;
    }
    case M_MSE: return t_pair_loss(vm, T_MSE, x, argv[1], m->reduction);
    case M_L1: return t_pair_loss(vm, T_L1, x, argv[1], m->reduction);
    case M_BCE: return t_pair_loss(vm, T_BCE, x, argv[1], m->reduction);
    case M_BCE_LOGITS: return t_pair_loss(vm, T_BCE_LOGITS, x, argv[1], m->reduction);
    default: return t_class_loss(vm, T_CROSS_ENTROPY, x, argv[1], m->reduction);
    }
}

/* ---------- parameters, modes, state ---------- */

/* Adds the module's parameters to list (a Sequential's from each of its modules: ours, or an object whose class
 * has a parameters method giving a list). 0 after an error (in *err). */
static int add_parameters(LubeVM *vm, LubeValue mod, LubeValue list, LubeValue *err) {
    Module *m = module_of(mod);
    if (!m) {
        LubeValue got = lube->call_method(vm, mod, "parameters", 0, NULL);
        if (lube->failed(got)) {
            *err = got;
            return 0;
        }
        if (lube->kind(got) != LUBE_LIST) {
            lube->release(got);
            *err = lube->error(vm, "parameters() must give a list of Tensors");
            return 0;
        }
        for (size_t i = 0; i < lube->list_len(got); i++) lube->list_push(list, lube->retain(lube->list_get(got, i)));
        lube->release(got);
        return 1;
    }
    if (lube->kind(m->weight) != LUBE_NOTHING) lube->list_push(list, lube->retain(m->weight));
    if (lube->kind(m->bias) != LUBE_NOTHING) lube->list_push(list, lube->retain(m->bias));
    for (size_t i = 0; m->kind == M_SEQUENTIAL && i < lube->list_len(m->children); i++)
        if (!add_parameters(vm, lube->list_get(m->children, i), list, err)) return 0;
    return 1;
}

static void set_training(Module *m, int on) {
    m->training = on;
    for (size_t i = 0; m->kind == M_SEQUENTIAL && i < lube->list_len(m->children); i++) {
        Module *c = module_of(lube->list_get(m->children, i));
        if (c) set_training(c, on);
    }
}

/* Adds name -> Tensor for the module's parameters to obj (prefix "0.", "1.weight"... in a Sequential). */
static void add_state(LubeVM *vm, Module *m, const char *prefix, LubeValue obj) {
    char name[512];
    if (lube->kind(m->weight) != LUBE_NOTHING) {
        snprintf(name, sizeof name, "%sweight", prefix);
        lube->object_set(vm, obj, name, lube->retain(m->weight));
    }
    if (lube->kind(m->bias) != LUBE_NOTHING) {
        snprintf(name, sizeof name, "%sbias", prefix);
        lube->object_set(vm, obj, name, lube->retain(m->bias));
    }
    for (size_t i = 0; m->kind == M_SEQUENTIAL && i < lube->list_len(m->children); i++) {
        Module *c = module_of(lube->list_get(m->children, i));
        if (!c) continue;
        snprintf(name, sizeof name, "%s%.0f.", prefix, (double)i);
        add_state(vm, c, name, obj);
    }
}

/* Copies what state has for each parameter (by the names state_dict gives) into it. */
static LubeValue load_state(LubeVM *vm, Module *m, const char *prefix, LubeValue state) {
    char name[512];
    LubeValue *slots[2] = {&m->weight, &m->bias};
    const char *names[2] = {"weight", "bias"};
    for (int s = 0; s < 2; s++) {
        if (lube->kind(*slots[s]) == LUBE_NOTHING) continue;
        snprintf(name, sizeof name, "%s%s", prefix, names[s]);
        LubeValue v;
        if (!lube->object_get(vm, state, name, &v)) return lube->error(vm, "load_state_dict(): no '%s' in the state", name);
        Tensor *t = tensor_of(*slots[s]);
        Array *have = arr->get(t->data), *src = array_of(v);
        if (!src || src->ndim != have->ndim || memcmp(src->shape, have->shape, sizeof(int64_t) * (size_t)have->ndim))
            return lube->error(vm, "load_state_dict(): '%s' has the wrong shape", name);
        LubeValue c = arr->from(vm, data_of(v), have->dtype), cc = arr->contiguous(vm, c, -1);
        lube->release(c);
        lube->release(t->data);
        t->data = cc;
    }
    for (size_t i = 0; m->kind == M_SEQUENTIAL && i < lube->list_len(m->children); i++) {
        Module *c = module_of(lube->list_get(m->children, i));
        if (!c) continue;
        snprintf(name, sizeof name, "%s%.0f.", prefix, (double)i);
        LubeValue r = load_state(vm, c, name, state);
        if (lube->failed(r)) return r;
    }
    return lube->nothing();
}

/* ======================================================================
 * Optimizers: SGD (momentum, nesterov, weight decay), Adam, AdamW
 * ==================================================================== */

enum { O_SGD, O_ADAM, O_ADAMW, O_COUNT };
static const char *const OPTIM_NAMES[O_COUNT] = {"SGD", "Adam", "AdamW"};
static LubeType *OptimTypes[O_COUNT];

typedef struct {
    int kind;
    LubeValue params;           /* its Tensors (a List) */
    LubeValue *m, *v;           /* per parameter: momentum or first moment, second moment (Arrays, or nothing) */
    size_t n;
    double lr, momentum, dampening, decay, b1, b2, eps;
    int nesterov;
    int64_t steps;
} Optim;

static Optim *optim_of(LubeValue v) {
    for (int i = 0; i < O_COUNT; i++) {
        Optim *o = (Optim *)lube->foreign(v, OptimTypes[i]);
        if (o) return o;
    }
    return NULL;  /* LCOV_EXCL_LINE: (its methods are only an optimizer's) */
}

static void optim_release(void *data) {
    Optim *o = (Optim *)data;
    lube->release(o->params);
    for (size_t i = 0; i < o->n; i++) {
        lube->release(o->m[i]);
        lube->release(o->v[i]);
    }
    free(o->m);
    free(o->v);
}

static int optim_describe(const void *data, char *buf, size_t size) {
    const Optim *o = (const Optim *)data;
    return snprintf(buf, size, "%s(%.0f parameters, lr %g)", OPTIM_NAMES[o->kind], (double)o->n, o->lr);
}

/* One parameter's update over elements from..to (a piece, so big parameters share out among threads). */
typedef struct {
    const Optim *o;
    int dtype, first;
    void *p, *m, *v;
    const void *g;
    double c1, c2;
} StepJob;

static void step_piece(void *ctx, int64_t from, int64_t to) {
    const StepJob *j = (const StepJob *)ctx;
    const Optim *o = j->o;
    BY_DTYPE(j->dtype, {
        T *p = (T *)j->p, *m = (T *)j->m, *v = (T *)j->v;
        const T *g = (const T *)j->g;
        T lr = (T)o->lr, decay = (T)o->decay, mom = (T)o->momentum, damp = (T)(1 - o->dampening);
        if (o->kind == O_SGD) {
            if (o->momentum == 0) {
                for (int64_t i = from; i < to; i++) p[i] -= lr * (g[i] + decay * p[i]);
            } else {
                for (int64_t i = from; i < to; i++) {
                    T gi = g[i] + decay * p[i];
                    m[i] = j->first ? gi : mom * m[i] + damp * gi;
                    p[i] -= lr * (o->nesterov ? gi + mom * m[i] : m[i]);
                }
            }
        } else {
            T b1 = (T)o->b1, b2 = (T)o->b2, a = (T)(o->lr / j->c1), s = (T)(1 / sqrt(j->c2)), eps = (T)o->eps;
            T wd = o->kind == O_ADAMW ? (T)(1 - o->lr * o->decay) : 1, l2 = o->kind == O_ADAMW ? 0 : decay;
            for (int64_t i = from; i < to; i++) {
                T gi = g[i] + l2 * p[i];
                m[i] = b1 * m[i] + (1 - b1) * gi;
                v[i] = b2 * v[i] + (1 - b2) * gi * gi;
                p[i] = p[i] * wd - a * m[i] / (sqrt(v[i]) * s + eps);
            }
        }
    });
}

/* One step: each parameter with a gradient moves against it (in place, gradients off). */
static void optim_step(LubeVM *vm, Optim *o) {
    o->steps++;
    double c1 = 1 - pow(o->b1, (double)o->steps), c2 = 1 - pow(o->b2, (double)o->steps);
    for (size_t i = 0; i < o->n; i++) {
        Tensor *t = tensor_of(lube->list_get(o->params, i));
        if (lube->kind(t->grad) == LUBE_NOTHING) continue;
        Array *d = arr->get(t->data);
        arr->own(d);
        LubeValue gc = arr->contiguous(vm, t->grad, d->dtype);
        void *mp;
        if (lube->kind(o->m[i]) == LUBE_NOTHING) {
            o->m[i] = arr->make(vm, d->dtype, d->ndim, d->shape, &mp);
            if (o->kind != O_SGD) o->v[i] = arr->make(vm, d->dtype, d->ndim, d->shape, &mp);
        }
        StepJob j = {o, d->dtype, o->steps == 1, d->data, arr->get(o->m[i])->data,
                     o->kind != O_SGD ? arr->get(o->v[i])->data : NULL, arr->get(gc)->data, c1, c2};
        arr->parallel(d->size, 1 << 16, step_piece, &j);
        lube->release(gc);
    }
}
