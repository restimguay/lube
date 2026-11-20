/*
 * nn.c - the plugin nn: Tensors with gradients (autograd), layers, losses
 * and optimizers, as PyTorch's torch and torch.nn.
 *
 *   import nn
 *   let x = nn.tensor([[1, 2], [3, 4]])          a Tensor: float32 unless asked; its data is an Array
 *   let w = nn.randn(2, 1, {requires_grad: true}) a leaf that wants gradients
 *   let loss = ((x @ w) ** 2).mean()              operators and methods record a graph
 *   loss.backward()                               ...and backward() gives each leaf its .grad
 *   let model = nn.Sequential([nn.Linear(2, 8), nn.ReLU(), nn.Linear(8, 1)])
 *   let opt = nn.Adam(model.parameters(), {lr: 0.01})
 *   opt.zero_grad(); nn.mse_loss(model.forward(x), y).backward(); opt.step()
 *
 * Layers: Linear, Conv2d, Embedding, LayerNorm, Dropout, the activations
 * (ReLU, Sigmoid, Tanh, GELU, SiLU, LeakyReLU, Softmax, LogSoftmax),
 * Flatten, MaxPool2d, AvgPool2d, Sequential (of these, and of objects of
 * your own classes with forward and parameters methods); losses as
 * functions (mse_loss, cross_entropy...) and as modules (MSELoss...);
 * optimizers SGD, Adam, AdamW. save and load write and read safetensors.
 * Arrays come from the plugin array (array.h), which nn uses.
 */
#include "array.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__  /* (no fused multiply-adds: the same results on every system) */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "f_tensor" -> "tensor" */

static const LubeAPI *lube;
static const ArrayExports *arr;

#include "nn/autograd.c"
#include "nn/ops.c"
#include "nn/modules.c"

/* ======================================================================
 * Arguments
 * ==================================================================== */

static int is_object(LubeValue v) { return lube->kind(v) == LUBE_OBJECT; }

static int option(LubeVM *vm, LubeValue opts, const char *key, LubeValue *out) {
    return is_object(opts) && lube->object_get(vm, opts, key, out);
}

static double opt_num(LubeVM *vm, LubeValue opts, const char *key, double fallback) {
    LubeValue v;
    return option(vm, opts, key, &v) && lube->kind(v) == LUBE_NUMBER ? lube->number(v) : fallback;
}

static int opt_bool(LubeVM *vm, LubeValue opts, const char *key, int fallback) {
    LubeValue v;
    return option(vm, opts, key, &v) && lube->kind(v) == LUBE_BOOL ? lube->truthy(v) : fallback;
}

/* Every key of opts is one of `known` (NULL-ended): 0 with an error naming the first that is not. */
static int known_options(LubeVM *vm, const char *fname, LubeValue opts, const char *const *known, LubeValue *err) {
    if (!is_object(opts)) return 1;
    LubeValue keys = lube->object_keys(vm, opts);
    int ok = 1;
    for (size_t i = 0; i < lube->list_len(keys) && ok; i++) {
        const char *k = lube->str(lube->list_get(keys, i), NULL);
        int found = 0;
        for (int j = 0; known[j] && k && !found; j++) found = !strcmp(known[j], k);
        if (!found) {
            *err = lube->error(vm, "%s(): there is no option '%s'", fname, k ? k : "?");
            ok = 0;
        }
    }
    lube->release(keys);
    return ok;
}

/* A dtype from options ({dtype: "float64"}): fallback when not given; -2 after an error. */
static int opt_dtype(LubeVM *vm, const char *fname, LubeValue opts, int fallback, LubeValue *err) {
    LubeValue v;
    if (!option(vm, opts, "dtype", &v)) return fallback;
    const char *s = lube->str(v, NULL);
    for (int t = 0; s && t < ARR_NDTYPES; t++)
        if (!strcmp(s, arr->dtype_names[t])) return t;
    *err = lube->error(vm, "%s(): there is no dtype %s (float32, float64, int64, int32, uint8, bool)", fname, s ? s : lube->a_type(v));
    return -2;
}

/* A dimension number of x (negative from the end): 0 after an error. */
static int dim_arg(LubeVM *vm, const char *fname, LubeValue x, LubeValue d, int *axis, LubeValue *err) {
    if (lube->kind(d) != LUBE_NUMBER) {
        *err = lube->error(vm, "%s(): a dimension is a whole Number, not %s", fname, lube->a_type(d));
        return 0;
    }
    return dim_of(vm, fname, x, lube->number(d), axis, err);
}

/* A shape from argv[from..]: Numbers, or one list of them; an options object after them in *opts. */
static int shape_args(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int from, int *nd, int64_t *shape,
                      LubeValue *opts, LubeValue *err) {
    *opts = lube->nothing();
    *nd = 0;
    int end = argc;
    if (end > from && is_object(argv[end - 1])) *opts = argv[--end];
    LubeValue src = end - from == 1 && lube->kind(argv[from]) == LUBE_LIST ? argv[from] : lube->nothing();
    int count = lube->kind(src) == LUBE_LIST ? (int)lube->list_len(src) : end - from;
    if (count > ARR_MAXDIM) {
        *err = lube->error(vm, "%s(): at most %d dimensions", fname, ARR_MAXDIM);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        LubeValue x = lube->kind(src) == LUBE_LIST ? lube->list_get(src, (size_t)i) : argv[from + i];
        double d = lube->number(x);
        if (lube->kind(x) != LUBE_NUMBER || d != floor(d) || d < 0) {
            *err = lube->error(vm, "%s(): a shape is whole numbers, 0 or more", fname);
            return 0;
        }
        shape[(*nd)++] = (int64_t)d;
    }
    return 1;
}

/* A Tensor of data as given (an Array: a view of its own; a Number, Bool or list: an Array of dtype). */
static LubeValue make_tensor(LubeVM *vm, LubeValue x, int dtype, int requires_grad) {
    LubeValue a;
    Tensor *t = tensor_of(x);
    Array *given = t ? arr->get(t->data) : arr->get(x);
    if (given) a = arr->from(vm, t ? t->data : x, dtype < 0 ? given->dtype : dtype);
    else a = arr->from(vm, x, dtype < 0 ? (lube->kind(x) == LUBE_BOOL ? ARR_BOOL : ARR_F32) : dtype);
    if (lube->failed(a)) return a;
    LubeValue view = arr->transpose(vm, a, NULL);  /* (an Array object of its own: optimizers change it in place) */
    Array *v = arr->get(view), *src = arr->get(a);
    for (int d = 0; d < v->ndim; d++) {  /* (the same order as before: a reversed view, reversed back) */
        v->shape[d] = src->shape[d];
        v->strides[d] = src->strides[d];
    }
    lube->release(a);
    if (requires_grad && v->dtype != ARR_F32 && v->dtype != ARR_F64) {
        lube->release(view);
        return lube->error(vm, "only Tensors of floats (float32, float64) can require gradients");
    }
    return tensor_new(vm, view, requires_grad, NULL);
}

static const char *const TENSOR_OPTIONS[] = {"requires_grad", "dtype", NULL};

/* ======================================================================
 * Making Tensors
 * ==================================================================== */

/* tensor(data, {requires_grad, dtype}) */
FN(f_tensor) {
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing(), err;
    if (!known_options(vm, FNAME, opts, TENSOR_OPTIONS, &err)) return err;
    int dtype = opt_dtype(vm, FNAME, opts, -1, &err);
    if (dtype < -1) return err;
    return make_tensor(vm, argv[0], dtype, opt_bool(vm, opts, "requires_grad", 0));
}

enum { FILL_ZEROS, FILL_ONES, FILL_RAND, FILL_RANDN };

static LubeValue filled(LubeVM *vm, const char *fname, int kind, int argc, const LubeValue *argv) {
    int nd;
    int64_t shape[ARR_MAXDIM];
    LubeValue opts, err;
    if (!shape_args(vm, fname, argc, argv, 0, &nd, shape, &opts, &err)) return err;
    if (!known_options(vm, fname, opts, TENSOR_OPTIONS, &err)) return err;
    int dtype = opt_dtype(vm, fname, opts, ARR_F32, &err);
    if (dtype < -1) return err;
    void *dp;
    LubeValue a = arr->make(vm, dtype, nd, shape, &dp);
    if (lube->failed(a)) return a;
    int64_t n = arr->get(a)->size;
    if (kind == FILL_ONES) {
        LubeValue one = arr->binary(vm, ARR_ADD, a, lube->number_value(1)), c = arr->from(vm, one, dtype);
        lube->release(one);
        lube->release(a);
        a = c;
    } else if (kind != FILL_ZEROS) {
        arr->fill_random(dp, dtype, n, kind == FILL_RANDN, 0, 1);
    }
    LubeValue r = make_tensor(vm, a, dtype, opt_bool(vm, opts, "requires_grad", 0));
    lube->release(a);
    return r;
}

FN(f_zeros) { return filled(vm, FNAME, FILL_ZEROS, argc, argv); }
FN(f_ones) { return filled(vm, FNAME, FILL_ONES, argc, argv); }
FN(f_rand) { return filled(vm, FNAME, FILL_RAND, argc, argv); }
FN(f_randn) { return filled(vm, FNAME, FILL_RANDN, argc, argv); }

/* arange(stop) or arange(start, stop, step?), then {requires_grad, dtype} */
FN(f_arange) {
    int n = argc;
    LubeValue opts = lube->nothing(), err;
    if (is_object(argv[n - 1])) opts = argv[--n];
    if (!known_options(vm, FNAME, opts, TENSOR_OPTIONS, &err)) return err;
    double v[3] = {0, 0, 1};
    if (n < 1 || n > 3) return lube->error(vm, "arange() takes stop, or start, stop and step (and options)");
    for (int i = 0; i < n; i++) {
        if (lube->kind(argv[i]) != LUBE_NUMBER) return lube->error(vm, "arange() expects Numbers, got %s", lube->a_type(argv[i]));
        v[n == 1 ? 1 : i] = lube->number(argv[i]);
    }
    int dtype = opt_dtype(vm, FNAME, opts, ARR_F32, &err);
    if (dtype < -1) return err;
    if (v[2] == 0) return lube->error(vm, "arange(): the step cannot be 0");
    double count = ceil((v[1] - v[0]) / v[2]);
    if (count > 1e12) return lube->error(vm, "arange(): that would be too long");
    int64_t len = count > 0 ? (int64_t)count : 0;
    void *dp;
    LubeValue a = arr->make(vm, ARR_F64, 1, &len, &dp);
    for (int64_t i = 0; i < len; i++) ((double *)dp)[i] = v[0] + (double)i * v[2];
    LubeValue r = make_tensor(vm, a, dtype, opt_bool(vm, opts, "requires_grad", 0));
    lube->release(a);
    return r;
}

/* full(shape, value, {requires_grad, dtype}) */
FN(f_full) {
    if (lube->kind(argv[1]) != LUBE_NUMBER) return lube->error(vm, "full() fills with a Number, not %s", lube->a_type(argv[1]));
    LubeValue z = filled(vm, FNAME, FILL_ZEROS, 1, argv);
    if (lube->failed(z)) return z;
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing(), err;
    if (!known_options(vm, FNAME, opts, TENSOR_OPTIONS, &err)) {
        lube->release(z);
        return err;
    }
    int dtype = opt_dtype(vm, FNAME, opts, ARR_F32, &err);
    if (dtype < -1) {
        lube->release(z);
        return err;
    }
    LubeValue s = arr->binary(vm, ARR_ADD, tensor_of(z)->data, argv[1]);
    LubeValue r = make_tensor(vm, s, dtype, opt_bool(vm, opts, "requires_grad", 0));
    lube->release(s);
    lube->release(z);
    return r;
}

/* manual_seed(n): array's generator (randn, rand, dropout and the layers' starting weights draw from it) */
FN(f_manual_seed) {
    (void)argc;
    double d = lube->number(argv[0]);
    if (lube->kind(argv[0]) != LUBE_NUMBER || d != floor(d) || d < 0 || d > 4294967295.0)
        return lube->error(vm, "manual_seed() takes a whole number in 0..4294967295");
    arr->seed((uint32_t)d);
    return lube->nothing();
}

/* no_grad(f): f() with gradients off (nothing it computes records a graph); what f returns */
FN(f_no_grad) {
    (void)argc;
    int before = grad_off;
    grad_off = 1;
    LubeValue r = lube->call(vm, argv[0], 0, NULL);
    grad_off = before;
    return r;
}

FN(f_set_grad_enabled) {
    (void)argc;
    if (lube->kind(argv[0]) != LUBE_BOOL) return lube->error(vm, "set_grad_enabled() takes true or false");
    grad_off = !lube->truthy(argv[0]);
    return lube->nothing();
}

FN(f_is_grad_enabled) {
    (void)argc;
    (void)argv;
    (void)vm;
    return lube->boolean(!grad_off);
}

/* ======================================================================
 * Functions of Tensors (also taking Arrays, Numbers and lists)
 * ==================================================================== */

#define BINARY_FN(name, op)                                                                                    \
    FN(f_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return t_binary(vm, op, argv[0], argv[1]);                                                             \
    }
BINARY_FN(add, T_ADD)
BINARY_FN(sub, T_SUB)
BINARY_FN(mul, T_MUL)
BINARY_FN(div, T_DIV)
BINARY_FN(pow, T_POW)

FN(f_matmul) {
    (void)argc;
    return t_matmul(vm, argv[0], argv[1]);
}

#define UNARY_FN(name, op)                                                                                     \
    FN(f_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return t_unary(vm, op, argv[0], 0, 0);                                                                 \
    }
UNARY_FN(exp, T_EXP)
UNARY_FN(log, T_LOG)
UNARY_FN(sqrt, T_SQRT)
UNARY_FN(tanh, T_TANH)
UNARY_FN(sigmoid, T_SIGMOID)
UNARY_FN(relu, T_RELU)
UNARY_FN(gelu, T_GELU)
UNARY_FN(silu, T_SILU)
UNARY_FN(abs, T_ABS)

FN(f_leaky_relu) {
    double slope = argc > 1 ? lube->number(argv[1]) : 0.01;
    if (argc > 1 && lube->kind(argv[1]) != LUBE_NUMBER) return lube->error(vm, "leaky_relu(): the slope is a Number");
    return t_unary(vm, T_LEAKY_RELU, argv[0], slope, 0);
}

FN(f_clamp) {
    (void)argc;
    if (lube->kind(argv[1]) != LUBE_NUMBER || lube->kind(argv[2]) != LUBE_NUMBER) return lube->error(vm, "clamp() takes the lowest and highest Numbers");
    return t_unary(vm, T_CLAMP, argv[0], lube->number(argv[1]), lube->number(argv[2]));
}

static LubeValue softmax_call(LubeVM *vm, int argc, const LubeValue *argv, int logs) {
    if (argc > 1 && lube->kind(argv[1]) != LUBE_NUMBER)
        return lube->error(vm, "%s(): a dimension is a whole Number, not %s", logs ? "log_softmax" : "softmax", lube->a_type(argv[1]));
    return t_softmax(vm, argv[0], argc > 1 ? lube->number(argv[1]) : -1, logs);
}

FN(f_softmax) { return softmax_call(vm, argc, argv, 0); }
FN(f_log_softmax) { return softmax_call(vm, argc, argv, 1); }

/* sum, mean and max of a Tensor: (dim?, {keepdim}) */
static LubeValue reduce_call(LubeVM *vm, const char *fname, int op, LubeValue x, int argc, const LubeValue *argv) {
    int axis = ARR_ALL_AXES, i = 1;
    LubeValue opts = lube->nothing(), err;
    if (argc > i && !is_object(argv[i])) {
        if (!dim_arg(vm, fname, x, argv[i], &axis, &err)) return err;
        i++;
    }
    if (argc > i) opts = argv[i];
    if (array_of(x)->size == 0 && op == T_MAX) return lube->error(vm, "max() of an empty Tensor");
    return t_reduce(vm, op, x, axis, opt_bool(vm, opts, "keepdim", 0));
}

/* A Tensor of anything (fname's argument): x itself (borrowed) or a new one in *made. Nothing after an error. */
static LubeValue tensor_arg(LubeVM *vm, LubeValue x, LubeValue *made, LubeValue *err) {
    *made = lube->nothing();
    if (tensor_of(x)) return x;
    *made = make_tensor(vm, x, -1, 0);
    if (lube->failed(*made)) {
        *err = *made;
        *made = lube->nothing();
        return lube->nothing();
    }
    return *made;
}

#define REDUCE_FN(name, op)                                                                                    \
    FN(f_##name) {                                                                                             \
        LubeValue made, err, x = tensor_arg(vm, argv[0], &made, &err);                                         \
        if (lube->kind(x) == LUBE_NOTHING) return err;                                                         \
        LubeValue r = reduce_call(vm, FNAME, op, x, argc, argv);                                               \
        lube->release(made);                                                                                   \
        return r;                                                                                              \
    }
REDUCE_FN(sum, T_SUM)
REDUCE_FN(mean, T_MEAN)
REDUCE_FN(max, T_MAX)

FN(f_cat) { return t_cat(vm, argv[0], argc > 1, argc > 1 ? lube->number(argv[1]) : 0, 0); }
FN(f_stack) { return t_cat(vm, argv[0], argc > 1, argc > 1 ? lube->number(argv[1]) : 0, 1); }

/* The reduction option of a loss ({reduction: "mean" | "sum" | "none"}): -1 after an error. */
static int reduction_of(LubeVM *vm, const char *fname, LubeValue opts, LubeValue *err) {
    LubeValue v;
    if (!option(vm, opts, "reduction", &v)) return RED_MEAN;
    const char *s = lube->str(v, NULL);
    if (s && !strcmp(s, "mean")) return RED_MEAN;
    if (s && !strcmp(s, "sum")) return RED_SUM;
    if (s && !strcmp(s, "none")) return RED_NONE;
    *err = lube->error(vm, "%s(): reduction is \"mean\", \"sum\" or \"none\"", fname);
    return -1;
}

static LubeValue loss_call(LubeVM *vm, const char *fname, int op, int argc, const LubeValue *argv) {
    LubeValue err;
    int red = reduction_of(vm, fname, argc > 2 ? argv[2] : lube->nothing(), &err);
    if (red < 0) return err;
    if (op == T_CROSS_ENTROPY || op == T_NLL) return t_class_loss(vm, op, argv[0], argv[1], red);
    return t_pair_loss(vm, op, argv[0], argv[1], red);
}

FN(f_mse_loss) { return loss_call(vm, FNAME, T_MSE, argc, argv); }
FN(f_l1_loss) { return loss_call(vm, FNAME, T_L1, argc, argv); }
FN(f_cross_entropy) { return loss_call(vm, FNAME, T_CROSS_ENTROPY, argc, argv); }
FN(f_nll_loss) { return loss_call(vm, FNAME, T_NLL, argc, argv); }
FN(f_binary_cross_entropy) { return loss_call(vm, FNAME, T_BCE, argc, argv); }
FN(f_binary_cross_entropy_with_logits) { return loss_call(vm, FNAME, T_BCE_LOGITS, argc, argv); }

/* dropout(x, p?, training?) */
FN(f_dropout) {
    double p = argc > 1 ? lube->number(argv[1]) : 0.5;
    if ((argc > 1 && lube->kind(argv[1]) != LUBE_NUMBER) || p < 0 || p >= 1) return lube->error(vm, "dropout(): p is a Number in 0..1 (not 1)");
    if (argc > 2 && lube->kind(argv[2]) != LUBE_BOOL) return lube->error(vm, "dropout(): training is true or false");
    LubeValue made, err, x = tensor_arg(vm, argv[0], &made, &err);
    if (lube->kind(x) == LUBE_NOTHING) return err;
    LubeValue r = t_dropout(vm, x, p, argc > 2 ? lube->truthy(argv[2]) : 1);
    lube->release(made);
    return r;
}

/* layer_norm(x, weight?, bias?, {eps}) */
FN(f_layer_norm) {
    LubeValue w = argc > 1 && !is_object(argv[1]) ? argv[1] : lube->nothing();
    LubeValue b = argc > 2 && !is_object(argv[2]) ? argv[2] : lube->nothing();
    LubeValue opts = argc > 1 && is_object(argv[argc - 1]) ? argv[argc - 1] : lube->nothing();
    return t_layer_norm(vm, argv[0], w, b, opt_num(vm, opts, "eps", 1e-5));
}

FN(f_embedding) {
    (void)argc;
    return t_embedding(vm, argv[0], argv[1]);
}

/* conv2d(x, weight, bias?, {stride, padding}) */
FN(f_conv2d) {
    LubeValue b = argc > 2 && !is_object(argv[2]) ? argv[2] : lube->nothing();
    LubeValue opts = argc > 2 && is_object(argv[argc - 1]) ? argv[argc - 1] : lube->nothing();
    return t_conv2d(vm, argv[0], argv[1], b, (int)opt_num(vm, opts, "stride", 1), (int)opt_num(vm, opts, "padding", 0));
}

static LubeValue pool_call(LubeVM *vm, int op, int argc, const LubeValue *argv) {
    if (lube->kind(argv[1]) != LUBE_NUMBER) return lube->error(vm, "pooling takes a kernel size (a Number)");
    int k = (int)lube->number(argv[1]);
    int s = argc > 2 ? (int)opt_num(vm, argv[2], "stride", k) : k;
    return t_pool(vm, op, argv[0], k, s);
}

FN(f_max_pool2d) { return pool_call(vm, T_MAX_POOL, argc, argv); }
FN(f_avg_pool2d) { return pool_call(vm, T_AVG_POOL, argc, argv); }

/* linear(x, weight, bias?): x weight^T + bias */
FN(f_linear) { return t_linear(vm, argv[0], argv[1], argc > 2 ? argv[2] : lube->nothing()); }

/* ======================================================================
 * Tensors' methods, properties and operators
 * ==================================================================== */

FN(m_backward) {
    LubeValue grad = argc > 1 && !is_object(argv[1]) ? argv[1] : lube->nothing();
    LubeValue opts = argc > 1 && is_object(argv[argc - 1]) ? argv[argc - 1] : lube->nothing();
    return backward(vm, argv[0], grad, opt_bool(vm, opts, "retain_graph", 0));
}

/* detach(): the same data, without the graph (and wanting no gradient) */
FN(m_detach) {
    (void)argc;
    return make_tensor(vm, tensor_of(argv[0])->data, -1, 0);
}

FN(m_item) {
    (void)argc;
    Array *a = arr->get(tensor_of(argv[0])->data);
    if (a->size != 1) return lube->error(vm, "item() of a Tensor of %.0f elements (it needs one)", (double)a->size);
    LubeValue c = arr->contiguous(vm, tensor_of(argv[0])->data, ARR_F64);
    double x = *(const double *)arr->get(c)->data;
    lube->release(c);
    return a->dtype == ARR_BOOL ? lube->boolean(x != 0) : lube->number_value(x);
}

/* numpy() / to_array(): the data, as an Array of its own */
FN(m_numpy) {
    (void)argc;
    LubeValue once = arr->transpose(vm, tensor_of(argv[0])->data, NULL), twice = arr->transpose(vm, once, NULL);
    lube->release(once);
    return twice;
}

FN(m_to_list) {
    (void)argc;
    LubeValue a = tensor_of(argv[0])->data;
    return lube->call_method(vm, a, "to_list", 0, NULL);
}

/* reshape(shape...) / view(shape...) */
FN(m_reshape) {
    int nd = 0;
    int64_t shape[ARR_MAXDIM];
    LubeValue src = argc == 2 && lube->kind(argv[1]) == LUBE_LIST ? argv[1] : lube->nothing();
    int count = lube->kind(src) == LUBE_LIST ? (int)lube->list_len(src) : argc - 1;
    if (count > ARR_MAXDIM) return lube->error(vm, "reshape(): at most %d dimensions", ARR_MAXDIM);
    for (int i = 0; i < count; i++) {
        LubeValue x = lube->kind(src) == LUBE_LIST ? lube->list_get(src, (size_t)i) : argv[1 + i];
        double d = lube->number(x);
        if (lube->kind(x) != LUBE_NUMBER || d != floor(d) || d < -1) return lube->error(vm, "reshape(): a shape is whole numbers (one may be -1)");
        shape[nd++] = (int64_t)d;
    }
    return t_reshape(vm, argv[0], nd, shape);
}

FN(m_flatten) {
    (void)argc;
    int64_t n = -1;
    return t_reshape(vm, argv[0], 1, &n);
}

/* transpose(d0, d1): those two dimensions swapped */
FN(m_transpose) {
    (void)argc;
    int a, b, axes[ARR_MAXDIM];
    LubeValue err;
    if (!dim_arg(vm, "transpose", argv[0], argv[1], &a, &err) || !dim_arg(vm, "transpose", argv[0], argv[2], &b, &err)) return err;
    int nd = array_of(argv[0])->ndim;
    for (int d = 0; d < nd; d++) axes[d] = d == a ? b : d == b ? a : d;
    return t_transpose(vm, argv[0], axes);
}

/* permute(dims...): the dimensions in that order */
FN(m_permute) {
    int nd = array_of(argv[0])->ndim, axes[ARR_MAXDIM], seen[ARR_MAXDIM] = {0};
    LubeValue src = argc == 2 && lube->kind(argv[1]) == LUBE_LIST ? argv[1] : lube->nothing();
    int count = lube->kind(src) == LUBE_LIST ? (int)lube->list_len(src) : argc - 1;
    if (count != nd) return lube->error(vm, "permute(): name each of the %d dimensions once", nd);
    LubeValue err;
    for (int i = 0; i < count; i++) {
        if (!dim_arg(vm, "permute", argv[0], lube->kind(src) == LUBE_LIST ? lube->list_get(src, (size_t)i) : argv[1 + i], &axes[i], &err)) return err;
        if (seen[axes[i]]++) return lube->error(vm, "permute(): name each of the %d dimensions once", nd);
    }
    return t_transpose(vm, argv[0], axes);
}

/* unsqueeze(d): a new dimension of length 1 at d */
FN(m_unsqueeze) {
    (void)argc;
    Array *a = array_of(argv[0]);
    double d = lube->number(argv[1]);
    if (lube->kind(argv[1]) != LUBE_NUMBER || d != floor(d) || d < -a->ndim - 1 || d > a->ndim || a->ndim == ARR_MAXDIM)
        return lube->error(vm, "unsqueeze(): no dimension %g for a Tensor of %d dimensions", d, a->ndim);
    int at = d < 0 ? (int)d + a->ndim + 1 : (int)d, k = 0;
    int64_t shape[ARR_MAXDIM];
    for (int i = 0; i <= a->ndim; i++) {
        if (i == at) shape[k++] = 1;
        if (i < a->ndim) shape[k++] = a->shape[i];
    }
    return t_reshape(vm, argv[0], a->ndim + 1, shape);
}

/* squeeze(d?): without dimensions of length 1 (or that one) */
FN(m_squeeze) {
    Array *a = array_of(argv[0]);
    int only = -1, nd = 0;
    LubeValue err;
    if (argc > 1 && !dim_arg(vm, "squeeze", argv[0], argv[1], &only, &err)) return err;
    int64_t shape[ARR_MAXDIM];
    for (int d = 0; d < a->ndim; d++)
        if (!(a->shape[d] == 1 && (only < 0 || only == d))) shape[nd++] = a->shape[d];
    return t_reshape(vm, argv[0], nd, shape);
}

FN(m_sum) { return reduce_call(vm, "sum", T_SUM, argv[0], argc, argv); }
FN(m_mean) { return reduce_call(vm, "mean", T_MEAN, argv[0], argc, argv); }
FN(m_max) { return reduce_call(vm, "max", T_MAX, argv[0], argc, argv); }

/* argmax(dim?): positions (an Array; no gradient) */
FN(m_argmax) {
    int axis = ARR_ALL_AXES;
    LubeValue err;
    if (argc > 1 && !dim_arg(vm, "argmax", argv[0], argv[1], &axis, &err)) return err;
    if (array_of(argv[0])->size == 0) return lube->error(vm, "argmax() of an empty Tensor");
    LubeValue r = arr->reduce(vm, ARR_ARGMAX, tensor_of(argv[0])->data, axis, 0);
    Array *a = arr->get(r);
    if (a->ndim) return r;
    LubeValue n = lube->number_value((double)*(const int64_t *)a->data);
    lube->release(r);
    return n;
}

#define UNARY_M(name, op)                                                                                      \
    FN(m_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return t_unary(vm, op, argv[0], 0, 0);                                                                 \
    }
UNARY_M(exp, T_EXP)
UNARY_M(log, T_LOG)
UNARY_M(sqrt, T_SQRT)
UNARY_M(tanh, T_TANH)
UNARY_M(sigmoid, T_SIGMOID)
UNARY_M(relu, T_RELU)
UNARY_M(abs, T_ABS)

FN(m_softmax) { return softmax_call(vm, argc, argv, 0); }
FN(m_log_softmax) { return softmax_call(vm, argc, argv, 1); }

FN(m_pow) {
    (void)argc;
    return t_binary(vm, T_POW, argv[0], argv[1]);
}

FN(m_matmul) {
    (void)argc;
    return t_matmul(vm, argv[0], argv[1]);
}

FN(m_clamp) { return f_clamp(vm, argc, argv); }

/* zero_grad(): no gradient (as PyTorch's set_to_none) */
FN(m_zero_grad) {
    (void)vm;
    (void)argc;
    Tensor *t = tensor_of(argv[0]);
    lube->release(t->grad);
    t->grad = lube->nothing();
    return lube->nothing();
}

/* requires_grad_(on?): the Tensor itself, wanting gradients (or not) */
FN(m_requires_grad_) {
    Tensor *t = tensor_of(argv[0]);
    int on = argc > 1 ? lube->truthy(argv[1]) : 1;
    if (argc > 1 && lube->kind(argv[1]) != LUBE_BOOL) return lube->error(vm, "requires_grad_() takes true or false");
    if (t->node) return lube->error(vm, "requires_grad_() is for leaves (this Tensor was computed: detach() it first)");
    int dt = arr->get(t->data)->dtype;
    if (on && dt != ARR_F32 && dt != ARR_F64) return lube->error(vm, "only Tensors of floats (float32, float64) can require gradients");
    t->requires_grad = on;
    return lube->retain(argv[0]);
}

/* In place, without a graph (as PyTorch's add_, sub_...): the Tensor itself. */
static LubeValue in_place(LubeVM *vm, LubeValue tv, int op, LubeValue x, double alpha) {
    Tensor *t = tensor_of(tv);
    if (t->node) return lube->error(vm, "changing a computed Tensor in place would spoil its graph (detach() it first)");
    LubeValue src = op == ARR_NBINARY ? x : lube->nothing();
    if (op != ARR_NBINARY) {
        LubeValue scaled = alpha == 1 ? lube->retain(data_of(x)) : arr->binary(vm, ARR_MUL, data_of(x), lube->number_value(alpha));
        if (lube->failed(scaled)) return scaled;
        src = arr->binary(vm, op, t->data, scaled);
        lube->release(scaled);
        if (lube->failed(src)) return src;
    } else {
        src = lube->retain(data_of(x));
    }
    Array *d = arr->get(t->data), *s = arr->get(src);
    LubeValue r = lube->retain(tv);
    if (!s || s->ndim > d->ndim || lube->kind(src) == LUBE_NUMBER) {  /* (a Number: everywhere) */
        LubeValue zero = arr->binary(vm, ARR_MUL, t->data, lube->number_value(0));
        LubeValue full = arr->binary(vm, ARR_ADD, zero, src);
        lube->release(zero);
        lube->release(src);
        src = full;
        s = arr->get(src);
    }
    LubeValue c = arr->from(vm, src, d->dtype);
    Array *ca = arr->get(c);
    if (!ca || ca->ndim != d->ndim || memcmp(ca->shape, d->shape, sizeof(int64_t) * (size_t)d->ndim)) {
        lube->release(r);
        r = lube->error(vm, "in place: the result's shape must be the Tensor's");
    } else {
        LubeValue cc = arr->contiguous(vm, c, d->dtype), view = arr->transpose(vm, cc, NULL);
        Array *v = arr->get(view), *cs = arr->get(cc);
        for (int k = 0; k < v->ndim; k++) {
            v->shape[k] = cs->shape[k];
            v->strides[k] = cs->strides[k];
        }
        lube->release(cc);
        lube->release(t->data);
        t->data = view;
    }
    lube->release(c);
    lube->release(src);
    return r;
}

static double alpha_of(int argc, const LubeValue *argv) { return argc > 2 ? lube->number(argv[2]) : 1; }

FN(m_add_) { return in_place(vm, argv[0], ARR_ADD, argv[1], alpha_of(argc, argv)); }
FN(m_sub_) { return in_place(vm, argv[0], ARR_SUB, argv[1], alpha_of(argc, argv)); }
FN(m_mul_) {
    (void)argc;
    return in_place(vm, argv[0], ARR_MUL, argv[1], 1);
}
FN(m_copy_) {
    (void)argc;
    return in_place(vm, argv[0], ARR_NBINARY, argv[1], 1);
}
FN(m_fill_) {
    (void)argc;
    if (lube->kind(argv[1]) != LUBE_NUMBER) return lube->error(vm, "fill_() takes a Number");
    return in_place(vm, argv[0], ARR_NBINARY, argv[1], 1);
}
FN(m_zero_) {
    (void)argc;
    return in_place(vm, argv[0], ARR_NBINARY, lube->number_value(0), 1);
}

/* astype(dtype): a copy of another dtype (no gradient) */
FN(m_astype) {
    (void)argc;
    LubeValue opts = lube->new_object(vm), err;
    lube->object_set(vm, opts, "dtype", lube->retain(argv[1]));
    int dt = opt_dtype(vm, "astype", opts, -1, &err);
    lube->release(opts);
    if (dt < -1) return err;
    return make_tensor(vm, tensor_of(argv[0])->data, dt, 0);
}

FN(p_shape) {
    (void)argc;
    Array *a = arr->get(tensor_of(argv[0])->data);
    LubeValue l = lube->new_list(vm);
    for (int d = 0; d < a->ndim; d++) lube->list_push(l, lube->number_value((double)a->shape[d]));
    return l;
}

FN(p_ndim) {
    (void)vm;
    (void)argc;
    return lube->number_value(arr->get(tensor_of(argv[0])->data)->ndim);
}

FN(p_dtype) {
    (void)argc;
    const char *n = arr->dtype_names[arr->get(tensor_of(argv[0])->data)->dtype];
    return lube->new_str(vm, n, strlen(n));
}

/* data: the Tensor's Array (a view of its own: changing it leaves the Tensor as it is) */
FN(p_data) {
    (void)argc;
    return m_numpy(vm, argc, argv);
}

/* grad: the gradient backward() gave (an Array), or zeros if there is none yet */
FN(p_grad) {
    (void)argc;
    Tensor *t = tensor_of(argv[0]);
    if (lube->kind(t->grad) != LUBE_NOTHING) return lube->retain(t->grad);
    return zeros_as(vm, arr->get(t->data), -1);
}

FN(p_requires_grad) {
    (void)vm;
    (void)argc;
    return lube->boolean(tensor_of(argv[0])->requires_grad);
}

FN(p_is_leaf) {
    (void)vm;
    (void)argc;
    return lube->boolean(tensor_of(argv[0])->node == NULL);
}

FN(p_has_grad) {
    (void)vm;
    (void)argc;
    return lube->boolean(lube->kind(tensor_of(argv[0])->grad) != LUBE_NOTHING);
}

FN(p_T) {
    (void)argc;
    return t_transpose(vm, argv[0], NULL);
}

#define OP_FN(name, op)                                                                                        \
    FN(o_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return t_binary(vm, op, argv[0], argv[1]);                                                             \
    }
OP_FN(add, T_ADD)
OP_FN(sub, T_SUB)
OP_FN(mul, T_MUL)
OP_FN(div, T_DIV)
OP_FN(pow, T_POW)

FN(o_matmul) {
    (void)argc;
    return t_matmul(vm, argv[0], argv[1]);
}

FN(o_neg) {
    (void)argc;
    return t_neg(vm, argv[0]);
}

/* comparisons: an Array of Bools (no gradient) */
#define CMP_FN(name, op)                                                                                       \
    FN(o_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return arr->binary(vm, op, data_of(argv[0]), data_of(argv[1]));                                        \
    }
CMP_FN(eq, ARR_EQ)
CMP_FN(ne, ARR_NE)
CMP_FN(lt, ARR_LT)
CMP_FN(le, ARR_LE)
CMP_FN(gt, ARR_GT)
CMP_FN(ge, ARR_GE)

FN(o_get) { return t_index(vm, argc, argv); }

FN(o_len) {
    (void)argc;
    Array *a = arr->get(tensor_of(argv[0])->data);
    if (!a->ndim) return lube->error(vm, "len() of a 0-dimensional Tensor (it has one element: item())");
    return lube->number_value((double)a->shape[0]);
}

static int tensor_describe(const void *data, char *buf, size_t size) {
    const Tensor *t = (const Tensor *)data;
    const Array *a = arr->get(t->data);
    char small[256];
    int n = arr->describe(a, small, sizeof small);
    char *text = small;
    if ((size_t)n >= sizeof small) {
        text = (char *)malloc((size_t)n + 1);
        if (!text) abort();  /* LCOV_EXCL_LINE: out of memory */
        arr->describe(a, text, (size_t)n + 1);
    }
    int r = snprintf(buf, size, "Tensor(%s%s%s)", text, a->dtype != ARR_F32 ? ", " : "", a->dtype != ARR_F32 ? arr->dtype_names[a->dtype] : "");
    if (t->requires_grad && (size_t)r < size) r += snprintf(buf + r, size - (size_t)r, "%s", "");
    if (t->requires_grad) {
        int extra = snprintf(NULL, 0, "%s", " requires_grad");
        if ((size_t)r > 0 && (size_t)(r - 1) < size) {  /* (put before the closing parenthesis) */
            if ((size_t)(r + extra) < size) {
                buf[r - 1] = '\0';
                snprintf(buf + r - 1, size - (size_t)(r - 1), ", requires_grad)");
            }
        }
        r += extra + 1;
    }
    if (text != small) free(text);
    return r;
}

/* ======================================================================
 * Modules
 * ==================================================================== */

static const char *const LAYER_OPTIONS[] = {"bias", "stride", "padding", "eps", "reduction", NULL};

/* An options argument (argv[i], if it is an object) checked against the known names. */
static int layer_options(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int i, LubeValue *opts, LubeValue *err) {
    *opts = argc > i && is_object(argv[i]) ? argv[i] : lube->nothing();
    if (argc > i && !is_object(argv[i])) {
        *err = lube->error(vm, "%s(): expected options (an object), got %s", fname, lube->a_type(argv[i]));
        return 0;
    }
    return known_options(vm, fname, *opts, LAYER_OPTIONS, err);
}

/* Positive whole Numbers argv[0..count-1], into out. */
static int sizes(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int count, int *out, LubeValue *err) {
    for (int i = 0; i < count; i++) {
        double d = i < argc ? lube->number(argv[i]) : 0;
        if (i >= argc || lube->kind(argv[i]) != LUBE_NUMBER || d != floor(d) || d < 1 || d > 1e8) {
            *err = lube->error(vm, "%s() takes %d sizes, whole Numbers of 1 or more", fname, count);
            return 0;
        }
        out[i] = (int)d;
    }
    return 1;
}

/* Linear(in, out, {bias}): weights uniform in +-1/sqrt(in), as PyTorch's */
FN(f_Linear) {
    int s[2];
    LubeValue opts, err;
    if (!sizes(vm, FNAME + 0, argc, argv, 2, s, &err) || !layer_options(vm, "Linear", argc, argv, 2, &opts, &err)) return err;
    Module *m;
    LubeValue v = module_new(vm, M_LINEAR, &m);
    m->a = s[0];
    m->b = s[1];
    double bound = 1 / sqrt((double)s[0]);
    int64_t ws[2] = {s[1], s[0]}, bs = s[1];
    m->weight = parameter(vm, 2, ws, bound, 0);
    if (opt_bool(vm, opts, "bias", 1)) m->bias = parameter(vm, 1, &bs, bound, 0);
    return v;
}

/* Conv2d(in, out, kernel, {stride, padding, bias}) */
FN(f_Conv2d) {
    int s[3];
    LubeValue opts, err;
    if (!sizes(vm, "Conv2d", argc, argv, 3, s, &err) || !layer_options(vm, "Conv2d", argc, argv, 3, &opts, &err)) return err;
    int stride = (int)opt_num(vm, opts, "stride", 1), pad = (int)opt_num(vm, opts, "padding", 0);
    if (stride < 1 || pad < 0) return lube->error(vm, "Conv2d(): stride is 1 or more, padding 0 or more");
    Module *m;
    LubeValue v = module_new(vm, M_CONV2D, &m);
    m->a = s[0];
    m->b = s[1];
    m->k = s[2];
    m->stride = stride;
    m->pad = pad;
    double bound = 1 / sqrt((double)s[0] * s[2] * s[2]);
    int64_t ws[4] = {s[1], s[0], s[2], s[2]}, bs = s[1];
    m->weight = parameter(vm, 4, ws, bound, 0);
    if (opt_bool(vm, opts, "bias", 1)) m->bias = parameter(vm, 1, &bs, bound, 0);
    return v;
}

/* Embedding(count, dim): weights normal (deviation 1), as PyTorch's */
FN(f_Embedding) {
    int s[2];
    LubeValue err, opts;
    if (!sizes(vm, "Embedding", argc, argv, 2, s, &err) || !layer_options(vm, "Embedding", argc, argv, 2, &opts, &err)) return err;
    Module *m;
    LubeValue v = module_new(vm, M_EMBEDDING, &m);
    m->a = s[0];
    m->b = s[1];
    int64_t ws[2] = {s[0], s[1]};
    m->weight = parameter(vm, 2, ws, 1, 1);
    return v;
}

/* LayerNorm(dim, {eps}): weight ones, bias zeros */
FN(f_LayerNorm) {
    int s[1];
    LubeValue opts, err;
    if (!sizes(vm, "LayerNorm", argc, argv, 1, s, &err) || !layer_options(vm, "LayerNorm", argc, argv, 1, &opts, &err)) return err;
    Module *m;
    LubeValue v = module_new(vm, M_LAYER_NORM, &m);
    m->a = s[0];
    m->p = opt_num(vm, opts, "eps", 1e-5);
    int64_t d = s[0];
    m->weight = parameter(vm, 1, &d, 0, 0);
    LubeValue ones = arr->binary(vm, ARR_ADD, tensor_of(m->weight)->data, lube->number_value(1));
    lube->release(tensor_of(m->weight)->data);
    tensor_of(m->weight)->data = ones;
    m->bias = parameter(vm, 1, &d, 0, 0);
    return v;
}

FN(f_Dropout) {
    double p = argc ? lube->number(argv[0]) : 0.5;
    if ((argc && lube->kind(argv[0]) != LUBE_NUMBER) || p < 0 || p >= 1) return lube->error(vm, "Dropout(): p is a Number in 0..1 (not 1)");
    Module *m;
    LubeValue v = module_new(vm, M_DROPOUT, &m);
    m->p = p;
    return v;
}

static LubeValue simple(LubeVM *vm, int kind) {
    Module *m;
    return module_new(vm, kind, &m);
}

FN(f_ReLU) {
    (void)argc;
    (void)argv;
    return simple(vm, M_RELU);
}
FN(f_Sigmoid) {
    (void)argc;
    (void)argv;
    return simple(vm, M_SIGMOID);
}
FN(f_Tanh) {
    (void)argc;
    (void)argv;
    return simple(vm, M_TANH);
}
FN(f_GELU) {
    (void)argc;
    (void)argv;
    return simple(vm, M_GELU);
}
FN(f_SiLU) {
    (void)argc;
    (void)argv;
    return simple(vm, M_SILU);
}
FN(f_Flatten) {
    (void)argc;
    (void)argv;
    return simple(vm, M_FLATTEN);
}

FN(f_LeakyReLU) {
    double slope = argc ? lube->number(argv[0]) : 0.01;
    if (argc && lube->kind(argv[0]) != LUBE_NUMBER) return lube->error(vm, "LeakyReLU(): the slope is a Number");
    Module *m;
    LubeValue v = module_new(vm, M_LEAKY_RELU, &m);
    m->p = slope;
    return v;
}

static LubeValue softmax_module(LubeVM *vm, int kind, int argc, const LubeValue *argv) {
    double d = argc ? lube->number(argv[0]) : -1;
    if (argc && (lube->kind(argv[0]) != LUBE_NUMBER || d != floor(d))) return lube->error(vm, "%s(): the dimension is a whole Number", MODULE_NAMES[kind]);
    Module *m;
    LubeValue v = module_new(vm, kind, &m);
    m->dim = (int)d;
    return v;
}

FN(f_Softmax) { return softmax_module(vm, M_SOFTMAX, argc, argv); }
FN(f_LogSoftmax) { return softmax_module(vm, M_LOG_SOFTMAX, argc, argv); }

static LubeValue pool_module(LubeVM *vm, int kind, int argc, const LubeValue *argv) {
    int s[1];
    LubeValue opts, err;
    if (!sizes(vm, MODULE_NAMES[kind], argc, argv, 1, s, &err) || !layer_options(vm, MODULE_NAMES[kind], argc, argv, 1, &opts, &err)) return err;
    Module *m;
    LubeValue v = module_new(vm, kind, &m);
    m->k = s[0];
    m->stride = (int)opt_num(vm, opts, "stride", s[0]);
    return v;
}

FN(f_MaxPool2d) { return pool_module(vm, M_MAX_POOL, argc, argv); }
FN(f_AvgPool2d) { return pool_module(vm, M_AVG_POOL, argc, argv); }

/* Sequential(modules): a list of them (ours, or objects with forward and parameters methods) */
FN(f_Sequential) {
    (void)argc;
    if (lube->kind(argv[0]) != LUBE_LIST) return lube->error(vm, "Sequential() takes a list of modules");
    for (size_t i = 0; i < lube->list_len(argv[0]); i++) {
        LubeValue c = lube->list_get(argv[0], i);
        if (!module_of(c) && lube->kind(c) != LUBE_OBJECT)
            return lube->error(vm, "Sequential(): %s is not a module (one of nn's, or an object with forward and parameters)", lube->a_type(c));
    }
    Module *m;
    LubeValue v = module_new(vm, M_SEQUENTIAL, &m);
    m->children = lube->retain(argv[0]);
    return v;
}

static LubeValue loss_module(LubeVM *vm, int kind, int argc, const LubeValue *argv) {
    LubeValue opts, err;
    if (!layer_options(vm, MODULE_NAMES[kind], argc, argv, 0, &opts, &err)) return err;
    int red = reduction_of(vm, MODULE_NAMES[kind], opts, &err);
    if (red < 0) return err;
    Module *m;
    LubeValue v = module_new(vm, kind, &m);
    m->reduction = red;
    return v;
}

FN(f_MSELoss) { return loss_module(vm, M_MSE, argc, argv); }
FN(f_L1Loss) { return loss_module(vm, M_L1, argc, argv); }
FN(f_CrossEntropyLoss) { return loss_module(vm, M_CROSS_ENTROPY, argc, argv); }
FN(f_BCELoss) { return loss_module(vm, M_BCE, argc, argv); }
FN(f_BCEWithLogitsLoss) { return loss_module(vm, M_BCE_LOGITS, argc, argv); }

FN(mm_forward) { return module_forward(vm, module_of(argv[0]), argc - 1, argv + 1); }

FN(mm_parameters) {
    (void)argc;
    LubeValue list = lube->new_list(vm), err;
    if (!add_parameters(vm, argv[0], list, &err)) {  /* LCOV_EXCL_START: (a Sequential checks its modules when made... */
        lube->release(list);
        return err;
    }  /* LCOV_EXCL_STOP */
    return list;
}

FN(mm_train) {
    if (argc > 1 && lube->kind(argv[1]) != LUBE_BOOL) return lube->error(vm, "train() takes true or false");
    set_training(module_of(argv[0]), argc > 1 ? lube->truthy(argv[1]) : 1);
    return lube->retain(argv[0]);
}

FN(mm_eval) {
    (void)argc;
    set_training(module_of(argv[0]), 0);
    return lube->retain(argv[0]);
}

FN(mm_state_dict) {
    (void)argc;
    LubeValue obj = lube->new_object(vm);
    add_state(vm, module_of(argv[0]), "", obj);
    return obj;
}

FN(mm_load_state_dict) {
    (void)argc;
    if (!is_object(argv[1])) return lube->error(vm, "load_state_dict() takes an object of Tensors by name");
    LubeValue r = load_state(vm, module_of(argv[0]), "", argv[1]);
    return lube->failed(r) ? r : lube->retain(argv[0]);
}

FN(mm_zero_grad) {
    (void)argc;
    LubeValue list = lube->new_list(vm), err;
    if (!add_parameters(vm, argv[0], list, &err)) {  /* LCOV_EXCL_START */
        lube->release(list);
        return err;
    }  /* LCOV_EXCL_STOP */
    for (size_t i = 0; i < lube->list_len(list); i++) {
        Tensor *t = tensor_of(lube->list_get(list, i));
        if (!t) continue;
        lube->release(t->grad);
        t->grad = lube->nothing();
    }
    lube->release(list);
    return lube->nothing();
}

FN(mp_weight) {
    (void)argc;
    Module *m = module_of(argv[0]);
    if (lube->kind(m->weight) == LUBE_NOTHING) return lube->error(vm, "%s has no weight", MODULE_NAMES[m->kind]);
    return lube->retain(m->weight);
}

FN(mp_bias) {
    (void)argc;
    Module *m = module_of(argv[0]);
    if (lube->kind(m->bias) == LUBE_NOTHING) return lube->error(vm, "%s has no bias", MODULE_NAMES[m->kind]);
    return lube->retain(m->bias);
}

FN(mp_training) {
    (void)vm;
    (void)argc;
    return lube->boolean(module_of(argv[0])->training);
}

/* ======================================================================
 * Optimizers
 * ==================================================================== */

static const char *const OPTIM_OPTIONS[] = {"lr", "momentum", "dampening", "weight_decay", "nesterov", "betas", "eps", NULL};

static LubeValue optim_new(LubeVM *vm, int kind, int argc, const LubeValue *argv) {
    const char *fname = OPTIM_NAMES[kind];
    LubeValue params = argv[0], opts = argc > 1 ? argv[1] : lube->nothing(), err, list;
    if (argc > 1 && !is_object(opts)) return lube->error(vm, "%s(): expected options (an object), got %s", fname, lube->a_type(opts));
    if (!known_options(vm, fname, opts, OPTIM_OPTIONS, &err)) return err;
    if (module_of(params)) {  /* a module: its parameters */
        list = lube->new_list(vm);
        if (!add_parameters(vm, params, list, &err)) {  /* LCOV_EXCL_START */
            lube->release(list);
            return err;
        }  /* LCOV_EXCL_STOP */
    } else if (lube->kind(params) == LUBE_LIST) {
        for (size_t i = 0; i < lube->list_len(params); i++)
            if (!tensor_of(lube->list_get(params, i))) return lube->error(vm, "%s(): the parameters must be Tensors", fname);
        list = lube->retain(params);
    } else {
        return lube->error(vm, "%s() takes a list of Tensors (or a module) and options", fname);
    }
    void *p;
    LubeValue v = lube->new_foreign(vm, OptimTypes[kind], &p);
    Optim *o = (Optim *)p;
    o->kind = kind;
    o->params = list;
    o->n = lube->list_len(list);
    o->m = (LubeValue *)malloc(sizeof(LubeValue) * (o->n + 1));
    o->v = (LubeValue *)malloc(sizeof(LubeValue) * (o->n + 1));
    if (!o->m || !o->v) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (size_t i = 0; i < o->n; i++) o->m[i] = o->v[i] = lube->nothing();
    o->lr = opt_num(vm, opts, "lr", kind == O_SGD ? 0.01 : 0.001);
    o->momentum = opt_num(vm, opts, "momentum", 0);
    o->dampening = opt_num(vm, opts, "dampening", 0);
    o->decay = opt_num(vm, opts, "weight_decay", kind == O_ADAMW ? 0.01 : 0);
    o->nesterov = opt_bool(vm, opts, "nesterov", 0);
    o->eps = opt_num(vm, opts, "eps", 1e-8);
    o->b1 = 0.9;
    o->b2 = 0.999;
    LubeValue betas;
    if (option(vm, opts, "betas", &betas)) {
        if (lube->kind(betas) != LUBE_LIST || lube->list_len(betas) != 2 || lube->kind(lube->list_get(betas, 0)) != LUBE_NUMBER ||
            lube->kind(lube->list_get(betas, 1)) != LUBE_NUMBER) {
            lube->release(v);
            return lube->error(vm, "%s(): betas is a list of two Numbers", fname);
        }
        o->b1 = lube->number(lube->list_get(betas, 0));
        o->b2 = lube->number(lube->list_get(betas, 1));
    }
    if (!(o->lr >= 0) || o->momentum < 0 || o->decay < 0 || (o->nesterov && o->momentum <= 0)) {
        lube->release(v);
        return lube->error(vm, "%s(): lr, momentum and weight_decay are 0 or more (nesterov needs momentum)", fname);
    }
    return v;
}

FN(f_SGD) { return optim_new(vm, O_SGD, argc, argv); }
FN(f_Adam) { return optim_new(vm, O_ADAM, argc, argv); }
FN(f_AdamW) { return optim_new(vm, O_ADAMW, argc, argv); }

FN(om_step) {
    (void)argc;
    optim_step(vm, optim_of(argv[0]));
    return lube->nothing();
}

FN(om_zero_grad) {
    (void)vm;
    (void)argc;
    Optim *o = optim_of(argv[0]);
    for (size_t i = 0; i < o->n; i++) {
        Tensor *t = tensor_of(lube->list_get(o->params, i));
        lube->release(t->grad);
        t->grad = lube->nothing();
    }
    return lube->nothing();
}

FN(om_set_lr) {
    (void)argc;
    if (lube->kind(argv[1]) != LUBE_NUMBER || !(lube->number(argv[1]) >= 0)) return lube->error(vm, "set_lr() takes a Number, 0 or more");
    optim_of(argv[0])->lr = lube->number(argv[1]);
    return lube->nothing();
}

FN(op_lr) {
    (void)vm;
    (void)argc;
    return lube->number_value(optim_of(argv[0])->lr);
}

/* ======================================================================
 * Files: safetensors
 * ==================================================================== */

/* save(path, tensors): an object of Tensors (or Arrays) by name, or a module (its state_dict) */
FN(f_save) {
    (void)argc;
    const char *path = lube->str(argv[0], NULL);
    if (!path) return lube->error(vm, "save() expects a path (a str) as argument 1, got %s", lube->a_type(argv[0]));
    LubeValue src = module_of(argv[1]) ? mm_state_dict(vm, 1, argv + 1) : lube->retain(argv[1]);
    if (!is_object(src)) {
        lube->release(src);
        return lube->error(vm, "save() takes an object of Tensors by name, or a module");
    }
    LubeValue keys = lube->object_keys(vm, src), arrays = lube->new_object(vm), r = lube->nothing();
    for (size_t i = 0; i < lube->list_len(keys); i++) {
        const char *k = lube->str(lube->list_get(keys, i), NULL);
        LubeValue v;
        lube->object_get(vm, src, k ? k : "", &v);
        if (!k || (!tensor_of(v) && !arr->get(v))) {
            r = lube->error(vm, "save(): every value must be a Tensor or an Array, named by a str");
            break;
        }
        lube->object_set(vm, arrays, k, lube->retain(data_of(v)));
    }
    if (!lube->failed(r)) r = arr->save_safetensors(vm, path, arrays);
    lube->release(keys);
    lube->release(arrays);
    lube->release(src);
    return r;
}

/* load(path): an object of Tensors by name (wanting no gradients) */
FN(f_load) {
    (void)argc;
    const char *path = lube->str(argv[0], NULL);
    if (!path) return lube->error(vm, "load() expects a path (a str) as argument 1, got %s", lube->a_type(argv[0]));
    LubeValue arrays = arr->load_safetensors(vm, path);
    if (lube->failed(arrays)) return arrays;
    LubeValue keys = lube->object_keys(vm, arrays), obj = lube->new_object(vm);
    for (size_t i = 0; i < lube->list_len(keys); i++) {
        const char *k = lube->str(lube->list_get(keys, i), NULL);
        LubeValue v;
        lube->object_get(vm, arrays, k, &v);
        lube->object_set(vm, obj, k, make_tensor(vm, v, -1, 0));
    }
    lube->release(keys);
    lube->release(arrays);
    return obj;
}

/* ======================================================================
 * The plugin
 * ==================================================================== */

LUBE_PLUGIN(nn) {
    lube = api;
    if (api->version < 2) return -1;  /* LCOV_EXCL_LINE */
    arr = (const ArrayExports *)api->require(plugin, "array");
    if (!arr || arr->version < ARRAY_EXPORTS) return -1;  /* LCOV_EXCL_LINE: (array comes with nn) */
    TensorType = api->type(plugin, "Tensor", sizeof(Tensor), tensor_release, tensor_describe);
    api->constructor(TensorType, f_tensor, 1, 2);
    struct { const char *name; LubeFn fn; int min, max; } fns[] = {
        {"tensor", f_tensor, 1, 2}, {"zeros", f_zeros, 0, -1}, {"ones", f_ones, 0, -1}, {"rand", f_rand, 0, -1},
        {"randn", f_randn, 0, -1}, {"arange", f_arange, 1, 4}, {"full", f_full, 2, 3}, {"manual_seed", f_manual_seed, 1, 1},
        {"no_grad", f_no_grad, 1, 1}, {"set_grad_enabled", f_set_grad_enabled, 1, 1}, {"is_grad_enabled", f_is_grad_enabled, 0, 0},
        {"add", f_add, 2, 2}, {"sub", f_sub, 2, 2}, {"mul", f_mul, 2, 2}, {"div", f_div, 2, 2}, {"pow", f_pow, 2, 2},
        {"matmul", f_matmul, 2, 2}, {"exp", f_exp, 1, 1}, {"log", f_log, 1, 1}, {"sqrt", f_sqrt, 1, 1}, {"tanh", f_tanh, 1, 1},
        {"sigmoid", f_sigmoid, 1, 1}, {"relu", f_relu, 1, 1}, {"gelu", f_gelu, 1, 1}, {"silu", f_silu, 1, 1}, {"abs", f_abs, 1, 1},
        {"leaky_relu", f_leaky_relu, 1, 2}, {"clamp", f_clamp, 3, 3}, {"softmax", f_softmax, 1, 2}, {"log_softmax", f_log_softmax, 1, 2},
        {"sum", f_sum, 1, 3}, {"mean", f_mean, 1, 3}, {"max", f_max, 1, 3}, {"cat", f_cat, 1, 2}, {"stack", f_stack, 1, 2},
        {"mse_loss", f_mse_loss, 2, 3}, {"l1_loss", f_l1_loss, 2, 3}, {"cross_entropy", f_cross_entropy, 2, 3},
        {"nll_loss", f_nll_loss, 2, 3}, {"binary_cross_entropy", f_binary_cross_entropy, 2, 3},
        {"binary_cross_entropy_with_logits", f_binary_cross_entropy_with_logits, 2, 3}, {"dropout", f_dropout, 1, 3},
        {"layer_norm", f_layer_norm, 1, 4}, {"embedding", f_embedding, 2, 2}, {"conv2d", f_conv2d, 2, 4},
        {"max_pool2d", f_max_pool2d, 2, 3}, {"avg_pool2d", f_avg_pool2d, 2, 3}, {"linear", f_linear, 2, 3},
        {"save", f_save, 2, 2}, {"load", f_load, 1, 1},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    struct { const char *name; LubeFn fn; int min, max; } tm[] = {
        {"backward", m_backward, 1, 3}, {"detach", m_detach, 1, 1}, {"item", m_item, 1, 1}, {"numpy", m_numpy, 1, 1},
        {"to_array", m_numpy, 1, 1}, {"to_list", m_to_list, 1, 1}, {"tolist", m_to_list, 1, 1}, {"reshape", m_reshape, 2, -1},
        {"view", m_reshape, 2, -1}, {"flatten", m_flatten, 1, 1}, {"transpose", m_transpose, 3, 3}, {"permute", m_permute, 2, -1},
        {"unsqueeze", m_unsqueeze, 2, 2}, {"squeeze", m_squeeze, 1, 2}, {"sum", m_sum, 1, 3}, {"mean", m_mean, 1, 3},
        {"max", m_max, 1, 3}, {"argmax", m_argmax, 1, 2}, {"exp", m_exp, 1, 1}, {"log", m_log, 1, 1}, {"sqrt", m_sqrt, 1, 1},
        {"tanh", m_tanh, 1, 1}, {"sigmoid", m_sigmoid, 1, 1}, {"relu", m_relu, 1, 1}, {"abs", m_abs, 1, 1},
        {"softmax", m_softmax, 1, 2}, {"log_softmax", m_log_softmax, 1, 2}, {"pow", m_pow, 2, 2}, {"matmul", m_matmul, 2, 2},
        {"clamp", m_clamp, 3, 3}, {"zero_grad", m_zero_grad, 1, 1}, {"requires_grad_", m_requires_grad_, 1, 2},
        {"add_", m_add_, 2, 3}, {"sub_", m_sub_, 2, 3}, {"mul_", m_mul_, 2, 2}, {"copy_", m_copy_, 2, 2}, {"fill_", m_fill_, 2, 2},
        {"zero_", m_zero_, 1, 1}, {"astype", m_astype, 2, 2},
    };
    for (size_t i = 0; i < sizeof tm / sizeof tm[0]; i++) api->method(TensorType, tm[i].name, tm[i].fn, tm[i].min, tm[i].max);
    api->property(TensorType, "shape", p_shape);
    api->property(TensorType, "ndim", p_ndim);
    api->property(TensorType, "dtype", p_dtype);
    api->property(TensorType, "data", p_data);
    api->property(TensorType, "grad", p_grad);
    api->property(TensorType, "has_grad", p_has_grad);
    api->property(TensorType, "requires_grad", p_requires_grad);
    api->property(TensorType, "is_leaf", p_is_leaf);
    api->property(TensorType, "T", p_T);
    struct { const char *op; LubeFn fn; } ops[] = {
        {"+", o_add}, {"-", o_sub}, {"*", o_mul}, {"/", o_div}, {"**", o_pow}, {"@", o_matmul}, {"neg", o_neg},
        {"==", o_eq}, {"!=", o_ne}, {"<", o_lt}, {"<=", o_le}, {">", o_gt}, {">=", o_ge}, {"[]", o_get}, {"len", o_len},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) api->op(TensorType, ops[i].op, ops[i].fn);
    LubeFn makers[M_COUNT] = {f_Linear, f_Conv2d, f_Embedding, f_LayerNorm, f_Dropout, f_ReLU, f_Sigmoid, f_Tanh, f_GELU,
                              f_SiLU, f_LeakyReLU, f_Softmax, f_LogSoftmax, f_Flatten, f_MaxPool2d, f_AvgPool2d, f_Sequential,
                              f_MSELoss, f_L1Loss, f_CrossEntropyLoss, f_BCELoss, f_BCEWithLogitsLoss};
    for (int k = 0; k < M_COUNT; k++) {
        LubeType *t = ModuleTypes[k] = api->type(plugin, MODULE_NAMES[k], sizeof(Module), module_release, module_describe);
        api->constructor(t, makers[k], 0, 4);
        api->method(t, "forward", mm_forward, 2, 3);
        api->method(t, "parameters", mm_parameters, 1, 1);
        api->method(t, "train", mm_train, 1, 2);
        api->method(t, "eval", mm_eval, 1, 1);
        api->method(t, "state_dict", mm_state_dict, 1, 1);
        api->method(t, "load_state_dict", mm_load_state_dict, 2, 2);
        api->method(t, "zero_grad", mm_zero_grad, 1, 1);
        api->property(t, "weight", mp_weight);
        api->property(t, "bias", mp_bias);
        api->property(t, "training", mp_training);
    }
    LubeFn optims[O_COUNT] = {f_SGD, f_Adam, f_AdamW};
    for (int k = 0; k < O_COUNT; k++) {
        LubeType *t = OptimTypes[k] = api->type(plugin, OPTIM_NAMES[k], sizeof(Optim), optim_release, optim_describe);
        api->constructor(t, optims[k], 1, 2);
        api->method(t, "step", om_step, 1, 1);
        api->method(t, "zero_grad", om_zero_grad, 1, 1);
        api->method(t, "set_lr", om_set_lr, 2, 2);
        api->property(t, "lr", op_lr);
    }
    return LUBE_PLUGIN_API;
}
