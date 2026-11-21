/*
 * array.c - the plugin array: n-dimensional Arrays of numbers, as numpy's.
 *
 *   import array as np                 (or import array.*)
 *   let a = np.array([[1, 2], [3, 4]]) an Array: float64 unless asked (dtype "float32", "int64", "int32",
 *   let b = np.zeros(2, 2)             "uint8", "bool"); a.shape, a.ndim, a.size, a.dtype, a.T
 *   a + b, a * 2, a @ b, a > 1         elementwise and broadcasting, as numpy's; a @ b: matmul
 *   a[0, 1], a[.., 0], a[a > 2]        indexing: numbers, ranges (views), lists and masks (copies)
 *   a.sum(0), np.mean(a), a.argmax()   reductions over everything or one axis
 *   np.solve(a, b), np.svd(a)          linear algebra
 *   np.seed(0), np.rand(3)             random Arrays: the numbers numpy gives for the same seed
 *   np.save("a.npy", a), np.load(...)  numpy's files, and text (loadtxt, savetxt)
 *
 * Arrays behave as values: a copy, a view or a slice changes apart from the
 * Array it came from (they share memory until one of them changes). The
 * matrix product uses the system's BLAS where there is one (Accelerate on
 * macOS, OpenBLAS elsewhere), else a kernel of our own; big loops run on
 * several threads. Other plugins (nn, learn, llm) use Arrays through the
 * table in array.h.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* sysconf under -std=c99 */
#endif
#include "array.h"

#ifdef __clang__  /* (no fused multiply-adds unless asked: the same results on every system; gcc's -std=c99 makes none) */
#pragma STDC FP_CONTRACT OFF
#define FUSED _Pragma("STDC FP_CONTRACT ON")
#else
#define FUSED
#endif
#include "vecmath.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>  /* (max and min of a row: NEON's own, which keep a NaN) */
#endif

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;

#include "array/core.c"
#include "array/pool.c"
#include "array/ops.c"
#include "array/index.c"
#include "array/linalg.c"
#include "array/random.c"
#include "array/io.c"

/* ======================================================================
 * Arguments
 * ==================================================================== */

/* An Array of anything array() takes, as fname's argument i: *keep is to be let go of. NULL after an error. */
static Array *array_arg(LubeVM *vm, const char *fname, LubeValue v, int i, LubeValue *keep) {
    Array *a;
    *keep = arr_from(vm, v, -1, &a);
    if (!a) {
        lube->release(*keep);
        *keep = lube->error(vm, "%s() expects an Array (or a list or Number) as argument %d, got %s", fname, i + 1,
                            lube->a_type(v));
        return NULL;
    }
    return a;
}

#define ARRAY_ARG(var, keep, i)                                                                                \
    LubeValue keep;                                                                                            \
    Array *var = array_arg(vm, FNAME, argv[i], i, &keep);                                                      \
    if (!var) return keep
#define FNAME (__func__ + 2)  /* "f_zeros" -> "zeros" */

static int is_object(LubeValue v) { return lube->kind(v) == LUBE_OBJECT; }

/* An option from an options object ({keepdims: true}): 1 if it is there (*out borrowed). */
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

static const char *opt_str(LubeVM *vm, LubeValue opts, const char *key, const char *fallback) {
    LubeValue v;
    return option(vm, opts, key, &v) && lube->kind(v) == LUBE_STR ? lube->str(v, NULL) : fallback;
}

/* A dtype named by v; -2 after an error (in *err). */
static int dtype_arg(LubeVM *vm, const char *fname, LubeValue v, LubeValue *err) {
    const char *s = lube->str(v, NULL);
    int t = s ? dtype_named(s) : -1;
    if (t < 0) {
        *err = lube->error(vm, "%s(): there is no dtype %s%s%s (float64, float32, int64, int32, uint8, bool)", fname,
                           s ? "\"" : "", s ? s : lube->a_type(v), s ? "\"" : "");
        return -2;
    }
    return t;
}

/* A shape: Numbers from argv[from], or one list of them; a str after them is a dtype (*dtype, else -1). */
static int shape_args(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int from, int *nd, int64_t *shape,
                      int *dtype, LubeValue *err) {
    *nd = 0;
    *dtype = -1;
    int end = argc;
    if (end > from && lube->kind(argv[end - 1]) == LUBE_STR) {
        if ((*dtype = dtype_arg(vm, fname, argv[end - 1], err)) < -1) return 0;
        end--;
    }
    if (end - from == 1 && lube->kind(argv[from]) == LUBE_LIST) {
        LubeValue l = argv[from];
        if (lube->list_len(l) > ARR_MAXDIM) {
            *err = lube->error(vm, "%s(): at most %d dimensions", fname, ARR_MAXDIM);
            return 0;
        }
        for (size_t i = 0; i < lube->list_len(l); i++) {
            LubeValue x = lube->list_get(l, i);
            double d = lube->number(x);
            if (lube->kind(x) != LUBE_NUMBER || d < 0 || d != floor(d) || d > (double)MAX_ELEMENTS) {
                *err = lube->error(vm, "%s(): a shape is whole numbers, 0 or more", fname);
                return 0;
            }
            shape[(*nd)++] = (int64_t)d;
        }
        return 1;
    }
    if (end - from > ARR_MAXDIM) {
        *err = lube->error(vm, "%s(): at most %d dimensions", fname, ARR_MAXDIM);
        return 0;
    }
    for (int i = from; i < end; i++) {
        double d = lube->number(argv[i]);
        if (lube->kind(argv[i]) != LUBE_NUMBER || d < 0 || d != floor(d) || d > (double)MAX_ELEMENTS) {
            *err = lube->error(vm, "%s(): a shape is whole numbers, 0 or more (or a list of them)", fname);
            return 0;
        }
        shape[(*nd)++] = (int64_t)d;
    }
    return 1;
}

/* An axis of a (negative from the end). 0 after an error. */
static int axis_of(LubeVM *vm, const char *fname, const Array *a, LubeValue v, int *axis, LubeValue *err) {
    double d = lube->number(v);
    if (lube->kind(v) != LUBE_NUMBER) {
        *err = lube->error(vm, "%s(): an axis is a whole Number, not %s", fname, lube->a_type(v));
        return 0;
    }
    if (d != floor(d) || d < -a->ndim || d >= a->ndim) {
        char num[64];
        lube->format_number(d, num, sizeof num);
        *err = lube->error(vm, "%s(): axis %s is not one of this Array's (it has %d)", fname, num, a->ndim);
        return 0;
    }
    *axis = d < 0 ? (int)d + a->ndim : (int)d;
    return 1;
}

/* fname(a, axis?, options?): the axis (or ARR_ALL_AXES) and options from argv[1..]. */
static int axis_opts(LubeVM *vm, const char *fname, const Array *a, int argc, const LubeValue *argv, int *axis,
                     LubeValue *opts, LubeValue *err) {
    *axis = ARR_ALL_AXES;
    *opts = lube->nothing();
    int i = 1;
    if (argc > i && !is_object(argv[i])) {
        if (!axis_of(vm, fname, a, argv[i], axis, err)) return 0;
        i++;
    }
    if (argc > i) {
        if (!is_object(argv[i])) {
            *err = lube->error(vm, "%s(): expected an axis and then options (an object), got %s", fname, lube->a_type(argv[i]));
            return 0;
        }
        *opts = argv[i];
    }
    return 1;
}

static int failed(LubeValue v) { return lube->failed(v); }

/* A 0-dimensional Array's one value as a Number (or Bool); anything else as it is. Takes v. */
static LubeValue scalar_out(LubeValue v) {
    Array *a = arr_of(v);
    if (!a || a->ndim) return v;
    LubeValue r = elem_value(a->data, a->dtype);
    lube->release(v);
    return r;
}

/* ======================================================================
 * Making Arrays
 * ==================================================================== */

/* array(x, dtype?) */
FN(f_array) {
    int dtype = -1;
    LubeValue err;
    if (argc > 1 && (dtype = dtype_arg(vm, FNAME, argv[1], &err)) < -1) return err;
    Array *a;
    LubeValue v = arr_from(vm, argv[0], dtype, &a);
    if (a && a == arr_of(argv[0])) {  /* (an Array given: a copy of its own, as numpy's array() makes) */
        lube->release(v);
        return arr_copy(vm, a, dtype, NULL);
    }
    return v;
}

/* asarray(x, dtype?): x itself when it is an Array of that dtype already */
FN(f_asarray) {
    int dtype = -1;
    LubeValue err;
    if (argc > 1 && (dtype = dtype_arg(vm, FNAME, argv[1], &err)) < -1) return err;
    Array *a;
    return arr_from(vm, argv[0], dtype, &a);
}

/* Every element of a (C-contiguous) set to value, as store() would set one: stored once, then copied along. */
static void fill_with(Array *a, double value) {
    if (!a->size) return;
    store(a->data, a->dtype, value);
    int64_t n = a->size;
    uint64_t x8, *d8 = (uint64_t *)a->data;
    uint32_t x4, *d4 = (uint32_t *)a->data;
    switch (ITEMSIZE[a->dtype]) {
    case 8:
        memcpy(&x8, d8, 8);
        for (int64_t i = 1; i < n; i++) d8[i] = x8;
        break;
    case 4:
        memcpy(&x4, d4, 4);
        for (int64_t i = 1; i < n; i++) d4[i] = x4;
        break;
    default: memset(a->data + 1, a->data[0], (size_t)(n - 1)); break;
    }
}

static LubeValue filled(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, double value) {
    int nd, dtype;
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    if (!shape_args(vm, fname, argc, argv, 0, &nd, shape, &dtype, &err)) return err;
    Array *a;
    LubeValue v = arr_new(vm, dtype < 0 ? ARR_F64 : dtype, nd, shape, &a, value == 0);
    if (a && value != 0) fill_with(a, value);
    return v;
}

FN(f_zeros) { return filled(vm, FNAME, argc, argv, 0); }
FN(f_ones) { return filled(vm, FNAME, argc, argv, 1); }

/* full(shape, value, dtype?) */
FN(f_full) {
    int dtype = -1;
    LubeValue err;
    if (argc > 2 && (dtype = dtype_arg(vm, FNAME, argv[2], &err)) < -1) return err;
    int k = lube->kind(argv[1]);
    if (k != LUBE_NUMBER && k != LUBE_BOOL) return lube->error(vm, "full() fills with a Number or a Bool, not %s", lube->a_type(argv[1]));
    double x = k == LUBE_BOOL ? lube->truthy(argv[1]) : lube->number(argv[1]);
    LubeValue r = filled(vm, FNAME, 1, argv, x == 0 ? 0 : x);
    Array *a = arr_of(r);
    if (a && (dtype >= 0 || k == LUBE_BOOL)) {
        LubeValue c = arr_copy(vm, a, dtype >= 0 ? dtype : ARR_BOOL, NULL);
        lube->release(r);
        r = c;
    }
    return r;
}

static LubeValue like(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int from, double value) {
    LubeValue keep, err;
    Array *a = array_arg(vm, fname, argv[0], 0, &keep);
    if (!a) return keep;
    int dtype = a->dtype;
    if (argc > from && (dtype = dtype_arg(vm, fname, argv[from], &err)) < -1) {
        lube->release(keep);
        return err;
    }
    Array *r;
    LubeValue v = arr_new(vm, dtype, a->ndim, a->shape, &r, value == 0);
    if (value != 0) fill_with(r, value);
    lube->release(keep);
    return v;
}

FN(f_zeros_like) { return like(vm, FNAME, argc, argv, 1, 0); }
FN(f_ones_like) { return like(vm, FNAME, argc, argv, 1, 1); }

FN(f_full_like) {
    if (lube->kind(argv[1]) != LUBE_NUMBER && lube->kind(argv[1]) != LUBE_BOOL)
        return lube->error(vm, "full_like() fills with a Number or a Bool, not %s", lube->a_type(argv[1]));
    double x = lube->kind(argv[1]) == LUBE_BOOL ? lube->truthy(argv[1]) : lube->number(argv[1]);
    return like(vm, FNAME, argc, argv, 2, x);
}

/* arange(stop) or arange(start, stop, step?), then a dtype if wanted */
FN(f_arange) {
    int dtype = ARR_F64, n = argc;
    LubeValue err;
    if (lube->kind(argv[n - 1]) == LUBE_STR) {
        if ((dtype = dtype_arg(vm, FNAME, argv[n - 1], &err)) < -1) return err;
        n--;
    }
    if (n == 0 || n > 3) return lube->error(vm, "arange() takes stop, or start, stop and step (and a dtype)");
    for (int i = 0; i < n; i++)
        if (lube->kind(argv[i]) != LUBE_NUMBER) return lube->error(vm, "arange() expects Numbers, got %s", lube->a_type(argv[i]));
    double start = n > 1 ? lube->number(argv[0]) : 0, stop = lube->number(argv[n > 1 ? 1 : 0]),
           step = n > 2 ? lube->number(argv[2]) : 1;
    if (step == 0 || step != step) return lube->error(vm, "arange(): the step cannot be 0");
    double count = ceil((stop - start) / step);
    if (!(count > 0)) count = 0;
    if (count > (double)MAX_ELEMENTS) return lube->error(vm, "arange(): that would be too long");
    int64_t len = (int64_t)count;
    Array *a;
    LubeValue v = arr_new(vm, dtype, 1, &len, &a, 0);
    if (a) {  /* (start + i step, as store() would set each: a loop for each dtype) */
        if (dtype == ARR_F64) {
            for (int64_t i = 0; i < len; i++) ((double *)a->data)[i] = start + (double)i * step;
        } else if (dtype == ARR_F32) {
            for (int64_t i = 0; i < len; i++) ((float *)a->data)[i] = (float)(start + (double)i * step);
        } else if (dtype == ARR_I64 && start == floor(start) && step == floor(step) && fabs(start) < 9e15 &&
                   fabs(start + (double)len * step) < 9e15) {
            int64_t s0 = (int64_t)start, st = (int64_t)step;
            for (int64_t i = 0; i < len; i++) ((int64_t *)a->data)[i] = s0 + i * st;
        } else {
            for (int64_t i = 0; i < len; i++) store(a->data + i * ITEMSIZE[dtype], dtype, start + (double)i * step);
        }
    }
    return v;
}

/* linspace(start, stop, num?, {endpoint}) and logspace(start, stop, num?, {endpoint, base}) */
static LubeValue spaced(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int logs) {
    LubeValue opts = argc > 2 && is_object(argv[argc - 1]) ? argv[argc - 1] : lube->nothing();
    int nargs = is_object(opts) ? argc - 1 : argc;
    for (int i = 0; i < nargs; i++)
        if (lube->kind(argv[i]) != LUBE_NUMBER) return lube->error(vm, "%s() expects Numbers, got %s", fname, lube->a_type(argv[i]));
    double a = lube->number(argv[0]), b = lube->number(argv[1]), num = nargs > 2 ? lube->number(argv[2]) : 50;
    if (num < 0 || num != floor(num) || num > (double)MAX_ELEMENTS) return lube->error(vm, "%s(): num must be a whole number, 0 or more", fname);
    int endpoint = opt_bool(vm, opts, "endpoint", 1);
    double base = opt_num(vm, opts, "base", 10);
    int64_t n = (int64_t)num;
    Array *r;
    LubeValue v = arr_new(vm, ARR_F64, 1, &n, &r, 0);
    double div = endpoint ? (double)(n - 1) : (double)n, step = div > 0 ? (b - a) / div : 0;
    double *d = (double *)r->data;
    for (int64_t i = 0; i < n; i++) d[i] = a + (double)i * step;
    if (endpoint && n > 1) d[n - 1] = b;
    if (logs)
        for (int64_t i = 0; i < n; i++) d[i] = pow(base, d[i]);
    return v;
}

FN(f_linspace) { return spaced(vm, FNAME, argc, argv, 0); }
FN(f_logspace) { return spaced(vm, FNAME, argc, argv, 1); }

/* eye(n, m?, k?, dtype?): ones on diagonal k */
FN(f_eye) {
    int dtype = ARR_F64, n = argc;
    LubeValue err;
    if (lube->kind(argv[n - 1]) == LUBE_STR) {
        if ((dtype = dtype_arg(vm, FNAME, argv[n - 1], &err)) < -1) return err;
        n--;
    }
    double v[3] = {0, -1, 0};
    for (int i = 0; i < n && i < 3; i++) {
        if (lube->kind(argv[i]) != LUBE_NUMBER || lube->number(argv[i]) != floor(lube->number(argv[i])))
            return lube->error(vm, "eye(): n, m and k are whole Numbers");
        v[i] = lube->number(argv[i]);
    }
    if (n > 3 || n < 1) return lube->error(vm, "eye() takes n, m and k (and a dtype)");
    if (v[1] < 0) v[1] = v[0];
    if (v[0] < 0 || v[1] < 0) return lube->error(vm, "eye(): sizes are 0 or more");
    int64_t shape[2] = {(int64_t)v[0], (int64_t)v[1]};
    Array *a;
    LubeValue r = arr_new(vm, dtype, 2, shape, &a, 1);
    if (!a) return r;
    for (int64_t i = 0; i < shape[0]; i++) {
        int64_t j = i + (int64_t)v[2];
        if (j >= 0 && j < shape[1]) store(a->data + (i * shape[1] + j) * ITEMSIZE[dtype], dtype, 1);
    }
    return r;
}

FN(f_identity) {
    LubeValue args[2] = {argv[0], argc > 1 ? argv[1] : lube->nothing()};
    if (argc > 1) return f_eye(vm, 2, args);  /* (n, dtype) */
    return f_eye(vm, 1, args);
}

/* diag(v, k?): a matrix with v on diagonal k, or a matrix's diagonal k */
FN(f_diag) {
    ARRAY_ARG(a, keep, 0);
    double kd = argc > 1 ? lube->number(argv[1]) : 0;
    if (argc > 1 && (lube->kind(argv[1]) != LUBE_NUMBER || kd != floor(kd))) {
        lube->release(keep);
        return lube->error(vm, "diag(): k must be a whole Number");
    }
    int64_t k = (int64_t)kd;
    LubeValue r;
    if (a->ndim == 1) {
        int64_t n = a->shape[0] + (k < 0 ? -k : k), shape[2] = {n, n};
        Array *m;
        r = arr_new(vm, a->dtype, 2, shape, &m, 1);
        for (int64_t i = 0; i < a->shape[0]; i++) {
            int64_t row = k >= 0 ? i : i - k, col = k >= 0 ? i + k : i;
            memcpy(m->data + (row * n + col) * ITEMSIZE[a->dtype], a->data + i * a->strides[0] * ITEMSIZE[a->dtype],
                   (size_t)ITEMSIZE[a->dtype]);
        }
    } else if (a->ndim == 2) {
        int64_t r0 = k >= 0 ? 0 : -k, c0 = k >= 0 ? k : 0;
        int64_t len = a->shape[0] - r0 < a->shape[1] - c0 ? a->shape[0] - r0 : a->shape[1] - c0;
        if (len < 0) len = 0;
        Array *d;
        r = arr_new(vm, a->dtype, 1, &len, &d, 0);
        for (int64_t i = 0; i < len; i++)
            memcpy(d->data + i * ITEMSIZE[a->dtype],
                   a->data + ((r0 + i) * a->strides[0] + (c0 + i) * a->strides[1]) * ITEMSIZE[a->dtype],
                   (size_t)ITEMSIZE[a->dtype]);
    } else {
        r = lube->error(vm, "diag() takes an Array of 1 or 2 dimensions");
    }
    lube->release(keep);
    return r;
}

/* from_bytes(bytes, dtype?, shape?): an Array of those bytes (1 dimension unless a shape is given) */
FN(f_from_bytes) {
    size_t len;
    const unsigned char *b = lube->bytes(argv[0], &len);
    if (!b) return lube->error(vm, "from_bytes() expects Bytes as argument 1, got %s", lube->a_type(argv[0]));
    int dtype = ARR_U8;
    LubeValue err;
    if (argc > 1 && (dtype = dtype_arg(vm, FNAME, argv[1], &err)) < -1) return err;
    if (len % (size_t)ITEMSIZE[dtype]) return lube->error(vm, "from_bytes(): %.0f bytes do not make whole %s values", (double)len, DTYPE_NAMES[dtype]);
    int64_t n = (int64_t)(len / (size_t)ITEMSIZE[dtype]);
    int nd = 1;
    int64_t shape[ARR_MAXDIM] = {n};
    if (argc > 2) {
        int dt;
        if (!shape_args(vm, FNAME, 1, argv + 2, 0, &nd, shape, &dt, &err)) return err;
        if (shape_size(nd, shape) != n) return lube->error(vm, "from_bytes(): that shape does not hold %.0f values", (double)n);
    }
    Array *a;
    LubeValue v = arr_new(vm, dtype, nd, shape, &a, 0);
    if (len) memcpy(a->data, b, len);
    if (dtype == ARR_BOOL)
        for (int64_t i = 0; i < n; i++) a->data[i] = a->data[i] != 0;
    return v;
}

/* ======================================================================
 * Shapes: views where they can be
 * ==================================================================== */

/* a with shape (one entry may be -1: whatever is left): a view if a is contiguous, else a copy. */
static LubeValue reshaped(LubeVM *vm, LubeValue av, int nd, const int64_t *shape) {
    Array *a = arr_of(av);
    int64_t want[ARR_MAXDIM], known = 1;
    int free_at = -1;
    for (int d = 0; d < nd; d++) {
        want[d] = shape[d];
        if (shape[d] == -1) {
            if (free_at >= 0) return lube->error(vm, "reshape(): only one axis can be -1");
            free_at = d;
        } else {
            known *= shape[d];
        }
    }
    if (free_at >= 0) {
        if (!known || a->size % known) return lube->error(vm, "reshape(): %.0f elements do not fit that shape", (double)a->size);
        want[free_at] = a->size / known;
    }
    if (shape_size(nd, want) != a->size) {
        char x[200], y[200];
        shape_text(x, sizeof x, a->ndim, a->shape);
        shape_text(y, sizeof y, nd, want);
        return lube->error(vm, "reshape(): an Array of shape %s cannot become %s", x, y);
    }
    Array *src;
    LubeValue keep = as_contiguous(vm, av, -1, &src);
    Array *v;
    LubeValue r = arr_view(vm, src, &v);
    v->ndim = nd;
    memcpy(v->shape, want, sizeof(int64_t) * (size_t)nd);
    c_strides(v);
    lube->release(keep);
    return r;
}

static LubeValue export_reshape(LubeVM *vm, LubeValue a, int nd, const int64_t *shape) { return reshaped(vm, a, nd, shape); }

/* a with its axes in another order (NULL: reversed); a view. */
static LubeValue transposed(LubeVM *vm, LubeValue av, const int *axes) {
    Array *a = arr_of(av), *v;
    LubeValue r = arr_view(vm, a, &v);
    for (int d = 0; d < a->ndim; d++) {
        int from = axes ? axes[d] : a->ndim - 1 - d;
        v->shape[d] = a->shape[from];
        v->strides[d] = a->strides[from];
    }
    return r;
}

static LubeValue reshape_call(LubeVM *vm, const char *fname, LubeValue av, int argc, const LubeValue *argv, int from) {
    int nd = 0;
    int64_t shape[ARR_MAXDIM];
    if (argc - from == 1 && lube->kind(argv[from]) == LUBE_LIST) {
        LubeValue l = argv[from];
        if (lube->list_len(l) > ARR_MAXDIM) return lube->error(vm, "%s(): at most %d dimensions", fname, ARR_MAXDIM);
        for (size_t i = 0; i < lube->list_len(l); i++) {
            LubeValue x = lube->list_get(l, i);
            if (lube->kind(x) != LUBE_NUMBER || lube->number(x) != floor(lube->number(x)) || lube->number(x) < -1)
                return lube->error(vm, "%s(): a shape is whole numbers (one may be -1)", fname);
            shape[nd++] = (int64_t)lube->number(x);
        }
    } else {
        if (argc - from > ARR_MAXDIM) return lube->error(vm, "%s(): at most %d dimensions", fname, ARR_MAXDIM);
        for (int i = from; i < argc; i++) {
            if (lube->kind(argv[i]) != LUBE_NUMBER || lube->number(argv[i]) != floor(lube->number(argv[i])) || lube->number(argv[i]) < -1)
                return lube->error(vm, "%s(): a shape is whole numbers (one may be -1)", fname);
            shape[nd++] = (int64_t)lube->number(argv[i]);
        }
    }
    return reshaped(vm, av, nd, shape);
}

FN(f_reshape) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = reshape_call(vm, FNAME, keep, argc, argv, 1);
    lube->release(keep);
    return r;
}

FN(m_reshape) { return reshape_call(vm, "reshape", argv[0], argc, argv, 1); }

FN(f_ravel) {
    ARRAY_ARG(a, keep, 0);
    int64_t n = a->size;
    LubeValue r = reshaped(vm, keep, 1, &n);
    lube->release(keep);
    return r;
}

FN(m_flatten) {  /* (always a copy of its own, as numpy's) */
    Array *a = arr_of(argv[0]), *c;
    LubeValue r = arr_copy(vm, a, -1, &c);
    c->ndim = 1;
    c->shape[0] = c->size;
    c->strides[0] = 1;
    return r;
}

/* transpose(a, axes?) */
static LubeValue transpose_call(LubeVM *vm, const char *fname, LubeValue av, int argc, const LubeValue *argv) {
    Array *a = arr_of(av);
    if (argc < 2) return transposed(vm, av, NULL);
    int axes[ARR_MAXDIM], seen[ARR_MAXDIM] = {0}, n = 0;
    LubeValue err;
    if (lube->kind(argv[1]) == LUBE_LIST) {
        if ((int)lube->list_len(argv[1]) != a->ndim)
            return lube->error(vm, "%s(): axes must name each of the %d axes once", fname, a->ndim);
        for (int i = 0; i < a->ndim; i++)
            if (!axis_of(vm, fname, a, lube->list_get(argv[1], (size_t)i), &axes[n++], &err)) return err;
    } else {
        if (argc - 1 != a->ndim) return lube->error(vm, "%s(): axes must name each of the %d axes once", fname, a->ndim);
        for (int i = 1; i < argc; i++)
            if (!axis_of(vm, fname, a, argv[i], &axes[n++], &err)) return err;
    }
    for (int i = 0; i < n; i++) {
        if (seen[axes[i]]) return lube->error(vm, "%s(): axes must name each of the %d axes once", fname, a->ndim);
        seen[axes[i]] = 1;
    }
    return transposed(vm, av, axes);
}

FN(f_transpose) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = transpose_call(vm, FNAME, keep, argc, argv);
    lube->release(keep);
    return r;
}

FN(m_transpose) { return transpose_call(vm, "transpose", argv[0], argc, argv); }

static LubeValue swap_call(LubeVM *vm, const char *fname, LubeValue av, const LubeValue *argv) {
    Array *a = arr_of(av);
    int i, j, axes[ARR_MAXDIM];
    LubeValue err;
    if (!axis_of(vm, fname, a, argv[1], &i, &err) || !axis_of(vm, fname, a, argv[2], &j, &err)) return err;
    for (int d = 0; d < a->ndim; d++) axes[d] = d == i ? j : d == j ? i : d;
    return transposed(vm, av, axes);
}

FN(f_swapaxes) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    (void)argc;
    LubeValue r = swap_call(vm, FNAME, keep, argv);
    lube->release(keep);
    return r;
}

FN(m_swapaxes) {
    (void)argc;
    return swap_call(vm, "swapaxes", argv[0], argv);
}

/* expand_dims(a, axis): a new axis of length 1 there */
FN(f_expand_dims) {
    (void)argc;
    ARRAY_ARG(a, keep, 0);
    double d = lube->number(argv[1]);
    if (lube->kind(argv[1]) != LUBE_NUMBER || d != floor(d) || d < -a->ndim - 1 || d > a->ndim || a->ndim == ARR_MAXDIM) {
        lube->release(keep);
        return lube->error(vm, "expand_dims(): no axis %g for an Array of %d dimensions", d, a->ndim);
    }
    int at = d < 0 ? (int)d + a->ndim + 1 : (int)d;
    Array *v;
    LubeValue r = arr_view(vm, a, &v);
    for (int k = a->ndim; k > at; k--) {
        v->shape[k] = a->shape[k - 1];
        v->strides[k] = a->strides[k - 1];
    }
    v->shape[at] = 1;
    v->strides[at] = 0;
    v->ndim++;
    lube->release(keep);
    return r;
}

/* squeeze(a, axis?): without its axes of length 1 (or that one) */
static LubeValue squeeze_call(LubeVM *vm, const char *fname, LubeValue av, int argc, const LubeValue *argv) {
    Array *a = arr_of(av), *v;
    int only = -1;
    LubeValue err;
    if (argc > 1) {
        if (!axis_of(vm, fname, a, argv[1], &only, &err)) return err;
        if (a->shape[only] != 1) return lube->error(vm, "%s(): axis %d has length %.0f, not 1", fname, only, (double)a->shape[only]);
    }
    LubeValue r = arr_view(vm, a, &v);
    v->ndim = 0;
    for (int d = 0; d < a->ndim; d++) {
        if (a->shape[d] == 1 && (only < 0 || d == only)) continue;
        v->shape[v->ndim] = a->shape[d];
        v->strides[v->ndim++] = a->strides[d];
    }
    return r;
}

FN(f_squeeze) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = squeeze_call(vm, FNAME, keep, argc, argv);
    lube->release(keep);
    return r;
}

FN(m_squeeze) { return squeeze_call(vm, "squeeze", argv[0], argc, argv); }

/* broadcast_to(a, shape): a view of a broadcast to that shape */
FN(f_broadcast_to) {
    ARRAY_ARG(a, keep, 0);
    int nd, dt;
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    if (!shape_args(vm, FNAME, argc, argv, 1, &nd, shape, &dt, &err)) {
        lube->release(keep);
        return err;
    }
    Array want;
    want.ndim = nd;
    memcpy(want.shape, shape, sizeof(int64_t) * (size_t)nd);
    int bnd;
    int64_t bshape[ARR_MAXDIM];
    if (!broadcast(a, &want, &bnd, bshape) || bnd != nd || memcmp(bshape, shape, sizeof(int64_t) * (size_t)nd)) {
        LubeValue e = broadcast_error(vm, a, &want);
        lube->release(keep);
        return e;
    }
    Array *v;
    LubeValue r = arr_view(vm, a, &v);
    broadcast_strides(a, nd, shape, v->strides);
    v->ndim = nd;
    memcpy(v->shape, shape, sizeof(int64_t) * (size_t)nd);
    set_size(v);
    lube->release(keep);
    return r;
}

/* flip(a, axis?): reversed along an axis (or all of them); a view */
FN(f_flip) {
    ARRAY_ARG(a, keep, 0);
    int only = ARR_ALL_AXES;
    LubeValue err;
    if (argc > 1 && !axis_of(vm, FNAME, a, argv[1], &only, &err)) {
        lube->release(keep);
        return err;
    }
    Array *v;
    LubeValue r = arr_view(vm, a, &v);
    for (int d = 0; d < a->ndim; d++)
        if ((only == ARR_ALL_AXES || d == only) && a->shape[d] > 0) {
            v->data += (a->shape[d] - 1) * a->strides[d] * ITEMSIZE[a->dtype];
            v->strides[d] = -a->strides[d];
        }
    lube->release(keep);
    return r;
}

/* concatenate(arrays, axis?): joined along an axis (0 unless given) */
static LubeValue joined(LubeVM *vm, const char *fname, LubeValue list, int axis_given, double axis_d, int stack) {
    if (lube->kind(list) != LUBE_LIST || !lube->list_len(list))
        return lube->error(vm, "%s() expects a list of Arrays, at least one", fname);
    size_t n = lube->list_len(list);
    LubeValue *keep = (LubeValue *)malloc(sizeof(LubeValue) * n);
    Array **parts = (Array **)malloc(sizeof(Array *) * n);
    if (!keep || !parts) abort();  /* LCOV_EXCL_LINE: out of memory */
    LubeValue r = lube->nothing();
    size_t made = 0;
    int dtype = -1;
    for (; made < n; made++) {
        parts[made] = array_arg(vm, fname, lube->list_get(list, made), 0, &keep[made]);
        if (!parts[made]) {
            r = keep[made];
            goto out;
        }
        dtype = dtype < 0 ? parts[made]->dtype : promote(dtype, parts[made]->dtype);
    }
    Array first = *parts[0];
    if (stack) {  /* a new axis of length 1 in each, then joined along it */
        if (first.ndim == ARR_MAXDIM) {
            r = lube->error(vm, "%s(): too many dimensions", fname);
            goto out;
        }
        first.ndim++;
    }
    int nd = first.ndim, axis = axis_given ? (int)axis_d : 0;
    if (axis_d != floor(axis_d) || axis < -nd || axis >= nd) {
        r = lube->error(vm, "%s(): no axis %g for Arrays of %d dimensions", fname, axis_d, nd);
        goto out;
    }
    if (axis < 0) axis += nd;
    int64_t shape[ARR_MAXDIM], total = 0;
    for (size_t i = 0; i < n; i++) {
        Array p = *parts[i];
        if (stack) {  /* (the view with the new axis) */
            for (int d = p.ndim; d > axis; d--) {
                p.shape[d] = p.shape[d - 1];
                p.strides[d] = p.strides[d - 1];
            }
            p.shape[axis] = 1;
            p.strides[axis] = 0;
            p.ndim++;
        }
        if (p.ndim != nd) {
            r = lube->error(vm, "%s(): the Arrays have different numbers of dimensions", fname);
            goto out;
        }
        for (int d = 0; d < nd; d++)
            if (d != axis && p.shape[d] != (i ? shape[d] : p.shape[d])) {
                r = lube->error(vm, "%s(): the Arrays' shapes differ along axis %d", fname, d);
                goto out;
            }
        if (!i) memcpy(shape, p.shape, sizeof(int64_t) * (size_t)nd);
        total += p.shape[axis];
    }
    shape[axis] = total;
    Array *dst;
    r = arr_new(vm, dtype, nd, shape, &dst, 0);
    int64_t at = 0;
    for (size_t i = 0; i < n; i++) {
        Array p = *parts[i];
        if (stack) {
            for (int d = p.ndim; d > axis; d--) {
                p.shape[d] = p.shape[d - 1];
                p.strides[d] = p.strides[d - 1];
            }
            p.shape[axis] = 1;
            p.strides[axis] = 0;
            p.ndim++;
        }
        Array slot = *dst;
        slot.data = dst->data + at * dst->strides[axis] * ITEMSIZE[dtype];
        slot.shape[axis] = p.shape[axis];
        set_size(&slot);
        if (slot.size) copy_into(&slot, &p);
        at += p.shape[axis];
    }
out:
    for (size_t i = 0; i < made; i++) lube->release(keep[i]);
    free(keep);
    free(parts);
    return r;
}

FN(f_concatenate) { return joined(vm, FNAME, argv[0], argc > 1, argc > 1 ? lube->number(argv[1]) : 0, 0); }
FN(f_stack) { return joined(vm, FNAME, argv[0], argc > 1, argc > 1 ? lube->number(argv[1]) : 0, 1); }

/* vstack (rows: 1-dimensional Arrays are rows) and hstack (columns) */
static LubeValue hv_stack(LubeVM *vm, const char *fname, LubeValue list, int vertical) {
    if (lube->kind(list) != LUBE_LIST || !lube->list_len(list))
        return lube->error(vm, "%s() expects a list of Arrays, at least one", fname);
    LubeValue first_keep;
    Array *first = array_arg(vm, fname, lube->list_get(list, 0), 0, &first_keep);
    if (!first) return first_keep;
    int one = first->ndim == 1;
    lube->release(first_keep);
    if (!vertical) return joined(vm, fname, list, 1, one ? 0 : 1, 0);
    if (!one) return joined(vm, fname, list, 1, 0, 0);
    return joined(vm, fname, list, 1, 0, 1);
}

FN(f_vstack) {
    (void)argc;
    return hv_stack(vm, FNAME, argv[0], 1);
}

FN(f_hstack) {
    (void)argc;
    return hv_stack(vm, FNAME, argv[0], 0);
}

/* tile(a, reps): a repeated, reps times along each axis (a Number: along the last) */
FN(f_tile) {
    (void)argc;
    ARRAY_ARG(a, keep, 0);
    int64_t reps[ARR_MAXDIM];
    int nr = 0, dt;
    LubeValue err;
    if (!shape_args(vm, FNAME, 1, argv + 1, 0, &nr, reps, &dt, &err)) {
        lube->release(keep);
        return err;
    }
    int nd = nr > a->ndim ? nr : a->ndim;
    if (2 * nd > ARR_MAXDIM) {
        lube->release(keep);
        return lube->error(vm, "tile(): too many dimensions");
    }
    int64_t shape[ARR_MAXDIM] = {0}, r2[ARR_MAXDIM], s2[ARR_MAXDIM], st[ARR_MAXDIM];
    for (int d = 0; d < nd; d++) {
        int ad = a->ndim - nd + d, rd = nr - nd + d;
        s2[d] = ad >= 0 ? a->shape[ad] : 1;
        st[d] = ad >= 0 ? a->strides[ad] : 0;
        r2[d] = rd >= 0 ? reps[rd] : 1;
        shape[d] = s2[d] * r2[d];
    }
    Array *dst;
    LubeValue r = arr_new(vm, a->dtype, nd, shape, &dst, 0);
    if (!dst) {
        lube->release(keep);
        return r;
    }
    Array dv = *dst, sv = *a;  /* (as [r0, s0, r1, s1...]) */
    dv.ndim = sv.ndim = 2 * nd;
    for (int d = 0; d < nd; d++) {
        dv.shape[2 * d] = r2[d];
        dv.shape[2 * d + 1] = s2[d];
        dv.strides[2 * d] = s2[d] * dst->strides[d];
        dv.strides[2 * d + 1] = dst->strides[d];
        sv.shape[2 * d] = 1;
        sv.shape[2 * d + 1] = s2[d];
        sv.strides[2 * d] = 0;
        sv.strides[2 * d + 1] = st[d];
    }
    if (dst->size) copy_into(&dv, &sv);
    lube->release(keep);
    return r;
}

/* repeat(a, n, axis?): each element n times (along an axis, or of the flattened Array) */
FN(f_repeat) {
    ARRAY_ARG(a, keep, 0);
    double n = lube->number(argv[1]);
    if (lube->kind(argv[1]) != LUBE_NUMBER || n < 0 || n != floor(n)) {
        lube->release(keep);
        return lube->error(vm, "repeat(): the count must be a whole Number, 0 or more");
    }
    int axis = ARR_ALL_AXES;
    LubeValue err, src = lube->retain(keep);
    if (argc > 2 && !axis_of(vm, FNAME, a, argv[2], &axis, &err)) {
        lube->release(keep);
        lube->release(src);
        return err;
    }
    if (axis == ARR_ALL_AXES) {
        int64_t flat = a->size;
        lube->release(src);
        src = reshaped(vm, keep, 1, &flat);
        axis = 0;
    }
    Array *s = arr_of(src);
    int64_t shape[ARR_MAXDIM];
    memcpy(shape, s->shape, sizeof(int64_t) * (size_t)s->ndim);
    shape[axis] *= (int64_t)n;
    Array *dst;
    LubeValue r = s->ndim == ARR_MAXDIM ? lube->error(vm, "repeat(): too many dimensions") : arr_new(vm, s->dtype, s->ndim, shape, &dst, 0);
    if (s->ndim < ARR_MAXDIM && dst && dst->size && is_contiguous(s)) {  /* (directly: each element, or block, n times) */
        int64_t len = s->shape[axis], inner = 1, outer = 1, times = (int64_t)n;
        for (int d = 0; d < s->ndim; d++) {
            if (d < axis) outer *= s->shape[d];
            if (d > axis) inner *= s->shape[d];
        }
        int z = ITEMSIZE[s->dtype];
        if (inner == 1 && z == 8) {
            const uint64_t *x = (const uint64_t *)s->data;
            uint64_t *o = (uint64_t *)dst->data;
            for (int64_t i = 0; i < outer * len; i++)
                for (int64_t j = 0; j < times; j++) o[i * times + j] = x[i];
        } else if (inner == 1 && z == 4) {
            const uint32_t *x = (const uint32_t *)s->data;
            uint32_t *o = (uint32_t *)dst->data;
            for (int64_t i = 0; i < outer * len; i++)
                for (int64_t j = 0; j < times; j++) o[i * times + j] = x[i];
        } else {
            size_t block = (size_t)(inner * z);
            for (int64_t i = 0; i < outer * len; i++)
                for (int64_t j = 0; j < times; j++) memcpy(dst->data + (i * times + j) * (int64_t)block, s->data + i * (int64_t)block, block);
        }
    } else if (s->ndim < ARR_MAXDIM && dst && dst->size) {
        Array dv = *dst, sv = *s;  /* (axis split into [length, n]) */
        dv.ndim = sv.ndim = s->ndim + 1;
        for (int d = s->ndim; d > axis; d--) {
            dv.shape[d] = dst->shape[d - 1];
            dv.strides[d] = dst->strides[d - 1];
            sv.shape[d] = s->shape[d - 1];
            sv.strides[d] = s->strides[d - 1];
        }
        dv.shape[axis] = s->shape[axis];
        dv.shape[axis + 1] = (int64_t)n;
        dv.strides[axis] = (int64_t)n * dst->strides[axis];
        dv.strides[axis + 1] = dst->strides[axis];
        sv.shape[axis + 1] = 1;
        sv.strides[axis + 1] = 0;
        copy_into(&dv, &sv);
    }
    lube->release(src);
    lube->release(keep);
    return r;
}

/* roll(a, shift, axis?): elements moved shift places along an axis (or the flattened Array), round the end */
FN(f_roll) {
    ARRAY_ARG(a, keep, 0);
    double sh = lube->number(argv[1]);
    if (lube->kind(argv[1]) != LUBE_NUMBER || sh != floor(sh)) {
        lube->release(keep);
        return lube->error(vm, "roll(): the shift must be a whole Number");
    }
    int axis = ARR_ALL_AXES;
    LubeValue err;
    if (argc > 2 && !axis_of(vm, FNAME, a, argv[2], &axis, &err)) {
        lube->release(keep);
        return err;
    }
    Array *c = a;
    LubeValue src = is_contiguous(a) ? lube->retain(keep) : arr_copy(vm, a, -1, &c), r;  /* (a copy only if need be) */
    Array *dst;
    r = arr_new(vm, a->dtype, a->ndim, a->shape, &dst, 0);
    int64_t outer = 1, n = a->size, inner = 1;
    if (axis != ARR_ALL_AXES) {
        n = a->shape[axis];
        for (int d = 0; d < a->ndim; d++) {
            if (d < axis) outer *= a->shape[d];
            if (d > axis) inner *= a->shape[d];
        }
    }
    int z = ITEMSIZE[a->dtype];
    int64_t s = n ? (((int64_t)sh % n) + n) % n : 0;
    for (int64_t o = 0; o < outer && n; o++) {  /* (two blocks: the first n - s go s on, the last s to the front) */
        const unsigned char *from = c->data + o * n * inner * z;
        unsigned char *to = dst->data + o * n * inner * z;
        memcpy(to + s * inner * z, from, (size_t)((n - s) * inner * z));
        memcpy(to, from + (n - s) * inner * z, (size_t)(s * inner * z));
    }
    lube->release(src);
    lube->release(keep);
    return r;
}

/* ======================================================================
 * Elementwise: functions of one Array, or two
 * ==================================================================== */

#define UNARY(name, op)                                                                                        \
    FN(f_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return un_op(vm, op, argv[0]);                                                                         \
    }
UNARY(negative, ARR_NEG)
UNARY(abs, ARR_ABS)
UNARY(sqrt, ARR_SQRT)
UNARY(exp, ARR_EXP)
UNARY(log2, ARR_LOG2)
UNARY(log10, ARR_LOG10)
UNARY(log1p, ARR_LOG1P)
UNARY(expm1, ARR_EXPM1)
UNARY(sin, ARR_SIN)
UNARY(cos, ARR_COS)
UNARY(tan, ARR_TAN)
UNARY(arcsin, ARR_ASIN)
UNARY(arccos, ARR_ACOS)
UNARY(arctan, ARR_ATAN)
UNARY(sinh, ARR_SINH)
UNARY(cosh, ARR_COSH)
UNARY(tanh, ARR_TANH)
UNARY(arcsinh, ARR_ASINH)
UNARY(arccosh, ARR_ACOSH)
UNARY(arctanh, ARR_ATANH)
UNARY(floor, ARR_FLOOR)
UNARY(ceil, ARR_CEIL)
UNARY(trunc, ARR_TRUNC)
UNARY(rint, ARR_RINT)
UNARY(sign, ARR_SIGN)
UNARY(square, ARR_SQUARE)
UNARY(reciprocal, ARR_RECIPROCAL)
UNARY(cbrt, ARR_CBRT)
UNARY(exp2, ARR_EXP2)
UNARY(erf, ARR_ERF)
UNARY(isnan, ARR_ISNAN)
UNARY(isinf, ARR_ISINF)
UNARY(isfinite, ARR_ISFINITE)
UNARY(logical_not, ARR_NOT)

/* log(x, base?) */
FN(f_log) {
    LubeValue r = un_op(vm, ARR_LOG, argv[0]);
    if (argc < 2 || failed(r)) return r;
    LubeValue lb = un_op(vm, ARR_LOG, argv[1]);
    if (failed(lb)) {
        lube->release(r);
        return lb;
    }
    LubeValue q = bin_op(vm, ARR_DIV, r, lb, 0, NULL);
    lube->release(r);
    lube->release(lb);
    return q;
}

#define BINARY(name, op)                                                                                       \
    FN(f_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return bin_op(vm, op, argv[0], argv[1], 0, NULL);                                                      \
    }
BINARY(add, ARR_ADD)
BINARY(subtract, ARR_SUB)
BINARY(multiply, ARR_MUL)
BINARY(divide, ARR_DIV)
BINARY(mod, ARR_MOD)
BINARY(power, ARR_POW)
BINARY(maximum, ARR_MAXIMUM)
BINARY(minimum, ARR_MINIMUM)
BINARY(arctan2, ARR_ATAN2)
BINARY(hypot, ARR_HYPOT)
BINARY(fmod, ARR_FMOD)
BINARY(floor_divide, ARR_FLOOR_DIVIDE)
BINARY(copysign, ARR_COPYSIGN)
BINARY(less, ARR_LT)
BINARY(less_equal, ARR_LE)
BINARY(greater, ARR_GT)
BINARY(greater_equal, ARR_GE)
BINARY(equal, ARR_EQ)
BINARY(not_equal, ARR_NE)
BINARY(logical_and, ARR_AND)
BINARY(logical_or, ARR_OR)
BINARY(logical_xor, ARR_XOR)

/* round(a, decimals?): half to even, as numpy's (and Python's) */
static LubeValue rounded(LubeVM *vm, LubeValue x, int argc, const LubeValue *argv) {
    double d = argc > 1 ? lube->number(argv[1]) : 0;
    if (argc > 1 && (lube->kind(argv[1]) != LUBE_NUMBER || d != floor(d)))
        return lube->error(vm, "round(): decimals must be a whole Number");
    if (d == 0) return un_op(vm, ARR_RINT, x);
    double scale = pow(10, d);
    LubeValue up = bin_op(vm, ARR_MUL, x, lube->number_value(scale), 0, NULL);
    if (failed(up)) return up;
    LubeValue rr = un_op(vm, ARR_RINT, up);
    lube->release(up);
    LubeValue r = bin_op(vm, ARR_DIV, rr, lube->number_value(scale), 0, NULL);
    lube->release(rr);
    return r;
}

FN(f_round) { return rounded(vm, argv[0], argc, argv); }
FN(m_round) { return rounded(vm, argv[0], argc, argv); }

/* clip(a, lo, hi) */
static LubeValue clipped(LubeVM *vm, LubeValue x, LubeValue lo, LubeValue hi) {
    LubeValue up = bin_op(vm, ARR_MAXIMUM, x, lo, 0, NULL);
    if (failed(up)) return up;
    LubeValue r = bin_op(vm, ARR_MINIMUM, up, hi, 0, NULL);
    lube->release(up);
    return r;
}

FN(f_clip) {
    (void)argc;
    return clipped(vm, argv[0], argv[1], argv[2]);
}

FN(m_clip) {
    (void)argc;
    return clipped(vm, argv[0], argv[1], argv[2]);
}

typedef struct {
    int dtype;
} WhereCtx;

#define WHERE_LOOP(T)                                                                                          \
    do {                                                                                                       \
        T *o = (T *)p[0];                                                                                      \
        const T *x = (const T *)p[2], *y = (const T *)p[3];                                                    \
        const unsigned char *c = p[1];                                                                         \
        if (st[0] == 1 && st[1] == 1 && st[2] == 1 && st[3] == 1)                                              \
            for (int64_t i = 0; i < n; i++) o[i] = c[i] ? x[i] : y[i];                                         \
        else if (st[0] == 1 && st[1] == 1 && st[2] == 1 && st[3] == 0)                                         \
            for (int64_t i = 0; i < n; i++) o[i] = c[i] ? x[i] : y[0];                                         \
        else                                                                                                   \
            for (int64_t i = 0; i < n; i++) o[i * st[0]] = c[i * st[1]] ? x[i * st[2]] : y[i * st[3]];         \
    } while (0)

static void where_run(void *ctx, int64_t n, unsigned char *const *p, const int64_t *st) {
    switch (ITEMSIZE[((const WhereCtx *)ctx)->dtype]) {  /* (by size: the bits are copied, whatever they mean) */
    case 8: WHERE_LOOP(uint64_t); break;
    case 4: WHERE_LOOP(uint32_t); break;
    default: WHERE_LOOP(unsigned char); break;
    }
}

/* nonzero(a): a list of Arrays of positions, one for each axis, where a is not 0 */
static LubeValue nonzero(LubeVM *vm, const Array *a) {
    Array *b;
    LubeValue kb = arr_copy(vm, a, ARR_BOOL, &b);  /* (each x != 0, in C order) */
    const unsigned char *m = b->data;
    int64_t count = 0;
    for (int64_t i = 0; i < a->size; i++) count += m[i];
    LubeValue r = lube->new_list(vm);
    int nd = a->ndim ? a->ndim : 1;
    Array *idx[ARR_MAXDIM];
    for (int d = 0; d < nd; d++) lube->list_push(r, arr_new(vm, ARR_I64, 1, &count, &idx[d], 0));
    int64_t at = 0;
    if (a->ndim == 1) {  /* (the positions themselves; each write at the next free place, up to the last one) */
        int64_t *o = (int64_t *)idx[0]->data, last = a->size - 1;
        while (last >= 0 && !m[last]) last--;
        for (int64_t i = 0; i <= last; i++) {
            o[at] = i;
            at += m[i];
        }
        lube->release(kb);
        return r;
    }
    for (int64_t i = 0; i < a->size; i++) {
        if (!m[i]) continue;
        int64_t rest = i;
        for (int d = a->ndim - 1; d >= 0; d--) {
            ((int64_t *)idx[d]->data)[at] = rest % a->shape[d];
            rest /= a->shape[d];
        }
        if (!a->ndim) ((int64_t *)idx[0]->data)[at] = 0;
        at++;
    }
    lube->release(kb);
    return r;
}

FN(f_nonzero) {
    (void)argc;
    ARRAY_ARG(a, keep, 0);
    LubeValue r = nonzero(vm, a);
    lube->release(keep);
    return r;
}

FN(m_nonzero) {
    (void)argc;
    return nonzero(vm, arr_of(argv[0]));
}

/* where(cond): nonzero(cond); where(cond, x, y): x where cond is true, else y */
FN(f_where) {
    if (argc == 2) return lube->error(vm, "where() takes a condition alone, or with both x and y");
    ARRAY_ARG(c, keep, 0);
    if (argc == 1) {
        LubeValue r = nonzero(vm, c);
        lube->release(keep);
        return r;
    }
    Operand ox, oy;
    LubeValue err;
    if (!operand_of(vm, argv[1], &ox, &err)) {
        lube->release(keep);
        return err;
    }
    if (!operand_of(vm, argv[2], &oy, &err)) {
        operand_done(&ox);
        lube->release(keep);
        return err;
    }
    int dtype = promote(operand_dtype(&ox, &oy), operand_dtype(&oy, &ox));
    Array *x, *y, *cb;
    LubeValue kx = operand_as(vm, &ox, dtype, &x), ky = operand_as(vm, &oy, dtype, &y), kc = arr_from(vm, keep, ARR_BOOL, &cb);
    int nd;
    int64_t shape[ARR_MAXDIM];
    Array xy;
    LubeValue r;
    if (!broadcast(x, y, &xy.ndim, xy.shape) || !broadcast(cb, &xy, &nd, shape)) {
        r = lube->error(vm, "where(): the condition, x and y do not broadcast together");
    } else {
        Array *dst;
        r = arr_new(vm, dtype, nd, shape, &dst, 0);
        LoopSpec s;
        s.nd = nd;
        for (int d = 0; d < nd; d++) {
            s.shape[d] = shape[d];
            s.st[0][d] = dst->strides[d];
        }
        broadcast_strides(cb, nd, shape, s.st[1]);
        broadcast_strides(x, nd, shape, s.st[2]);
        broadcast_strides(y, nd, shape, s.st[3]);
        unsigned char *base[4] = {dst->data, cb->data, x->data, y->data};
        int size[4] = {ITEMSIZE[dtype], 1, ITEMSIZE[dtype], ITEMSIZE[dtype]};
        Loop L;
        loop_make(&L, &s, 4, base, size);
        WhereCtx w = {dtype};
        if (dst->size) loop_go(&L, where_run, &w, 1);
    }
    lube->release(kx);
    lube->release(ky);
    lube->release(kc);
    operand_done(&ox);
    operand_done(&oy);
    lube->release(keep);
    return r;
}

/* isclose(a, b, {rtol, atol, equal_nan}): |a - b| <= atol + rtol |b|, elementwise */
static LubeValue close_to(LubeVM *vm, int argc, const LubeValue *argv) {
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing();
    double rtol = opt_num(vm, opts, "rtol", 1e-5), atol = opt_num(vm, opts, "atol", 1e-8);
    int equal_nan = opt_bool(vm, opts, "equal_nan", 0);
    Operand oa, ob;
    LubeValue err;
    if (!operand_of(vm, argv[0], &oa, &err)) return err;
    if (!operand_of(vm, argv[1], &ob, &err)) {
        operand_done(&oa);
        return err;
    }
    Array *a, *b;
    LubeValue ka = operand_as(vm, &oa, ARR_F64, &a), kb = operand_as(vm, &ob, ARR_F64, &b);
    int nd;
    int64_t shape[ARR_MAXDIM];
    LubeValue r;
    if (!broadcast(a, b, &nd, shape)) {
        r = broadcast_error(vm, a, b);
    } else {
        Array *dst;
        r = arr_new(vm, ARR_BOOL, nd, shape, &dst, 0);
        Array av = *a, bv = *b;
        broadcast_strides(a, nd, shape, av.strides);
        broadcast_strides(b, nd, shape, bv.strides);
        av.ndim = bv.ndim = nd;
        memcpy(av.shape, shape, sizeof(int64_t) * (size_t)nd);
        memcpy(bv.shape, shape, sizeof(int64_t) * (size_t)nd);
        int flat_a = is_contiguous(&av), flat_b = is_contiguous(&bv), one_b = 1;
        for (int d = 0; d < nd; d++) one_b &= bv.strides[d] == 0;
        if (flat_a && (flat_b || one_b)) {  /* (in a row, against a row or one Number: directly) */
            const double *A = (const double *)av.data, *B = (const double *)bv.data;
            int64_t sb = one_b ? 0 : 1;
            for (int64_t i = 0; i < dst->size; i++) {
                double x = A[i], y = B[i * sb];
                int c = x == y || fabs(x - y) <= atol + rtol * fabs(y);
                if (x != x || y != y) c = equal_nan && x != x && y != y;
                dst->data[i] = (unsigned char)c;
            }
        } else {
            for (int64_t i = 0; i < dst->size; i++) {
                double x = *(const double *)elem_at(&av, i), y = *(const double *)elem_at(&bv, i);
                int c = x == y || fabs(x - y) <= atol + rtol * fabs(y);
                if (x != x || y != y) c = equal_nan && x != x && y != y;
                dst->data[i] = (unsigned char)c;
            }
        }
        if (!oa.a && !ob.a) r = scalar_out(r);
    }
    lube->release(ka);
    lube->release(kb);
    operand_done(&oa);
    operand_done(&ob);
    return r;
}

FN(f_isclose) { return close_to(vm, argc, argv); }

FN(f_allclose) {
    LubeValue c = close_to(vm, argc, argv);
    Array *a = arr_of(c);
    if (!a) return lube->kind(c) == LUBE_BOOL ? c : c;
    int all = 1;
    for (int64_t i = 0; i < a->size && all; i++) all = a->data[i];
    lube->release(c);
    return lube->boolean(all);
}

/* array_equal(a, b): the same shape and the same elements */
FN(f_array_equal) {
    (void)argc;
    LubeValue ka, kb;
    Array *a = array_arg(vm, FNAME, argv[0], 0, &ka);
    if (!a) return ka;
    Array *b = array_arg(vm, FNAME, argv[1], 1, &kb);
    if (!b) {
        lube->release(ka);
        return kb;
    }
    int same = a->ndim == b->ndim && !memcmp(a->shape, b->shape, sizeof(int64_t) * (size_t)a->ndim);
    for (int64_t i = 0; i < a->size && same; i++) same = load(elem_at(a, i), a->dtype) == load(elem_at(b, i), b->dtype);
    lube->release(ka);
    lube->release(kb);
    return lube->boolean(same);
}

/* ======================================================================
 * Reductions: functions (of anything array() takes) and methods
 * ==================================================================== */

static LubeValue reduce_call(LubeVM *vm, const char *fname, int op, LubeValue av, int argc, const LubeValue *argv) {
    Array *a = arr_of(av);
    int axis;
    LubeValue opts, err;
    if (!axis_opts(vm, fname, a, argc, argv, &axis, &opts, &err)) return err;
    int keepdims = opt_bool(vm, opts, "keepdims", 0), ddof = (int)opt_num(vm, opts, "ddof", 0);
    return scalar_out(reduce_op(vm, op, av, axis, keepdims, ddof));
}

#define REDUCTION_FN(name, op)                                                                                 \
    FN(f_##name) {                                                                                             \
        ARRAY_ARG(a, keep, 0);                                                                                 \
        (void)a;                                                                                               \
        LubeValue r = reduce_call(vm, FNAME, op, keep, argc, argv);                                            \
        lube->release(keep);                                                                                   \
        return r;                                                                                              \
    }
#define REDUCTION(name, op)                                                                                    \
    REDUCTION_FN(name, op)                                                                                     \
    FN(m_##name) { return reduce_call(vm, #name, op, argv[0], argc, argv); }
REDUCTION(sum, ARR_SUM)
REDUCTION(prod, ARR_PROD)
REDUCTION(mean, ARR_MEAN)
REDUCTION(argmin, ARR_ARGMIN)
REDUCTION(argmax, ARR_ARGMAX)
REDUCTION(any, ARR_ANY)
REDUCTION(all, ARR_ALL)
REDUCTION(var, ARR_VAR)
REDUCTION(std, ARR_STD)
REDUCTION_FN(amin, ARR_MIN)
REDUCTION_FN(amax, ARR_MAX)

/* min and max: of an Array (as amin, amax), or of Numbers given one by one (as Lube's own min and max) */
static LubeValue min_max(LubeVM *vm, const char *fname, int op, int argc, const LubeValue *argv) {
    if (argc > 1 && lube->kind(argv[0]) == LUBE_NUMBER && lube->kind(argv[1]) == LUBE_NUMBER) {
        double best = lube->number(argv[0]);
        for (int i = 1; i < argc; i++) {
            if (lube->kind(argv[i]) != LUBE_NUMBER) return lube->error(vm, "%s() of Numbers: got %s", fname, lube->a_type(argv[i]));
            double x = lube->number(argv[i]);
            if (op == ARR_MAX ? x > best : x < best) best = x;
        }
        return lube->number_value(best);
    }
    LubeValue keep;
    Array *a = array_arg(vm, fname, argv[0], 0, &keep);
    if (!a) return keep;
    LubeValue r = reduce_call(vm, fname, op, keep, argc, argv);
    lube->release(keep);
    return r;
}

FN(f_min) { return min_max(vm, FNAME, ARR_MIN, argc, argv); }
FN(f_max) { return min_max(vm, FNAME, ARR_MAX, argc, argv); }
FN(m_min) { return reduce_call(vm, "min", ARR_MIN, argv[0], argc, argv); }
FN(m_max) { return reduce_call(vm, "max", ARR_MAX, argv[0], argc, argv); }

/* ptp(a, axis?): max - min */
FN(f_ptp) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue hi = reduce_call(vm, FNAME, ARR_MAX, keep, argc, argv);
    if (failed(hi)) {
        lube->release(keep);
        return hi;
    }
    LubeValue lo = reduce_call(vm, FNAME, ARR_MIN, keep, argc, argv);
    LubeValue r = bin_op(vm, ARR_SUB, hi, lo, 0, NULL);
    lube->release(hi);
    lube->release(lo);
    lube->release(keep);
    return r;
}

static LubeValue cum_call(LubeVM *vm, const char *fname, int prod, LubeValue av, int argc, const LubeValue *argv) {
    int axis = ARR_ALL_AXES;
    LubeValue err;
    if (argc > 1 && !axis_of(vm, fname, arr_of(av), argv[1], &axis, &err)) return err;
    return cumulate(vm, prod, av, axis);
}

FN(f_cumsum) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = cum_call(vm, FNAME, 0, keep, argc, argv);
    lube->release(keep);
    return r;
}

FN(f_cumprod) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = cum_call(vm, FNAME, 1, keep, argc, argv);
    lube->release(keep);
    return r;
}

FN(m_cumsum) { return cum_call(vm, "cumsum", 0, argv[0], argc, argv); }
FN(m_cumprod) { return cum_call(vm, "cumprod", 1, argv[0], argc, argv); }

/* count_nonzero(a, axis?) */
FN(f_count_nonzero) {
    ARRAY_ARG(a, keep, 0);
    if (argc < 2 && is_contiguous(a)) {  /* (of everything, in a row: counted where it is) */
        int64_t c = 0, n = a->size, i = 0;
        if (ITEMSIZE[a->dtype] == 1) {  /* (bytes: 240 at a time, counted in a byte, sixteen or more side by side) */
            for (; i + 240 <= n; i += 240) {
                unsigned char k = 0;
                for (int j = 0; j < 240; j++) k += a->data[i + j] != 0;
                c += k;
            }
        }
        BY_ANY(a->dtype, {
            const T *x = (const T *)a->data;
            for (; i < n; i++) c += x[i] != 0;
        });
        lube->release(keep);
        return lube->number_value((double)c);
    }
    Array *b;
    LubeValue c = arr_copy(vm, a, ARR_BOOL, &b);  /* (as bools, each x != 0) */
    LubeValue r = reduce_call(vm, FNAME, ARR_SUM, c, argc, argv);
    lube->release(c);
    lube->release(keep);
    return r;
}

/* average(a, axis?, {weights}) */
FN(f_average) {
    ARRAY_ARG(a, keep, 0);
    int axis;
    LubeValue opts, err, w;
    if (!axis_opts(vm, FNAME, a, argc, argv, &axis, &opts, &err)) {
        lube->release(keep);
        return err;
    }
    if (!option(vm, opts, "weights", &w)) {
        LubeValue r = scalar_out(reduce_op(vm, ARR_MEAN, keep, axis, 0, 0));
        lube->release(keep);
        return r;
    }
    LubeValue kw;
    Array *wa = array_arg(vm, FNAME, w, 1, &kw);
    if (!wa) {
        lube->release(keep);
        return kw;
    }
    LubeValue wv = kw;
    if (axis != ARR_ALL_AXES && wa->ndim == 1 && a->ndim > 1) {  /* weights along the axis: shaped to broadcast */
        int64_t shape[ARR_MAXDIM];
        for (int d = 0; d < a->ndim; d++) shape[d] = d == axis ? wa->shape[0] : 1;
        wv = reshaped(vm, kw, a->ndim, shape);  /* (its own length: it fits) */
        lube->release(kw);
    }
    LubeValue prod = bin_op(vm, ARR_MUL, keep, wv, 0, NULL), r;
    if (!arr_of(prod)) {
        r = prod;
    } else {
        LubeValue top = reduce_op(vm, ARR_SUM, prod, axis, 0, 0);
        LubeValue bottom = reduce_op(vm, ARR_SUM, wv, axis == ARR_ALL_AXES || arr_of(wv)->ndim == a->ndim ? axis : ARR_ALL_AXES, 0, 0);
        r = scalar_out(bin_op(vm, ARR_DIV, top, bottom, 0, NULL));
        lube->release(top);
        lube->release(bottom);
        lube->release(prod);
    }
    lube->release(wv);
    lube->release(keep);
    return r;
}

/* ======================================================================
 * Sorting and searching
 * ==================================================================== */

#define LESS_NAN(a, b) ((a) < (b) || ((b) != (b) && (a) == (a)))  /* (NaN goes last, as numpy sorts it) */

/* a with axis moved last, as a C-contiguous copy (of dtype): rows of `*n` along that axis. */
static LubeValue axis_last(LubeVM *vm, const Array *a, int axis, int dtype, Array **out, int64_t *n) {
    int axes[ARR_MAXDIM], k = 0;
    for (int d = 0; d < a->ndim; d++)
        if (d != axis) axes[k++] = d;
    axes[k] = axis;
    Array moved;
    permuted(a, axes, &moved);
    *n = a->ndim ? a->shape[axis] : 1;
    return arr_copy(vm, &moved, dtype, out);
}

/* The inverse of axis_last: rows back in place, as a C-contiguous Array. Takes v. */
static LubeValue axis_back(LubeVM *vm, LubeValue v, int axis) {
    Array *a = arr_of(v);
    int axes[ARR_MAXDIM];
    for (int d = 0, k = 0; d < a->ndim; d++) axes[d] = d == axis ? a->ndim - 1 : k++;
    Array moved;
    permuted(a, axes, &moved);
    Array *c;
    LubeValue r = arr_copy(vm, &moved, -1, &c);
    lube->release(v);
    return r;
}

/* A sort key for a value: unsigned integers in the values' order (NaN last, after inf). */
static uint64_t sort_key(const unsigned char *p, int dtype) {
    if (dtype == ARR_I64) return (uint64_t)*(const int64_t *)p ^ 0x8000000000000000ull;
    double x = load(p, dtype);
    if (x != x) return UINT64_MAX;
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u >> 63 ? ~u : u | 0x8000000000000000ull;
}

static double key_value(uint64_t k) {  /* (the inverse, for floats) */
    if (k == UINT64_MAX) return NAN;
    uint64_t u = k >> 63 ? k & 0x7fffffffffffffffull : ~k;
    double x;
    memcpy(&x, &u, sizeof x);
    return x;
}

/* Sorts keys (and idx with them) by an LSD radix sort, a byte at a time, skipping bytes all keys share; keeps
 * equal keys in order. kt, it: room for n more of each. */
static void radix_sort(uint64_t *key, int64_t *idx, int64_t n, uint64_t *kt, int64_t *it) {
    static const int BYTES = 8;
    int64_t (*count)[256] = (int64_t (*)[256])calloc((size_t)BYTES, sizeof *count);
    if (!count) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t i = 0; i < n; i++)
        for (int b = 0; b < BYTES; b++) count[b][(key[i] >> (8 * b)) & 0xff]++;
    for (int b = 0; b < BYTES; b++) {
        if (count[b][(key[0] >> (8 * b)) & 0xff] == n) continue;  /* (every key has this byte) */
        int64_t at = 0;
        for (int d = 0; d < 256; d++) {
            int64_t c = count[b][d];
            count[b][d] = at;
            at += c;
        }
        for (int64_t i = 0; i < n; i++) {
            int64_t to = count[b][(key[i] >> (8 * b)) & 0xff]++;
            kt[to] = key[i];
            it[to] = idx[i];
        }
        memcpy(key, kt, sizeof(uint64_t) * (size_t)n);
        memcpy(idx, it, sizeof(int64_t) * (size_t)n);
    }
    free(count);
}

/* Sorts a short run of keys (and idx with them), keeping equal keys in order: insertion-sorted runs of 16, then
 * merged (a radix sort's passes cost more than that for a few hundred). kt, it: room for n more of each. */
static void short_sort(uint64_t *key, int64_t *idx, int64_t n, uint64_t *kt, int64_t *it) {
    for (int64_t lo = 0; lo < n; lo += 16) {
        int64_t hi = lo + 16 < n ? lo + 16 : n;
        for (int64_t i = lo + 1; i < hi; i++) {
            uint64_t k = key[i];
            int64_t x = idx[i], j = i;
            for (; j > lo && key[j - 1] > k; j--) {
                key[j] = key[j - 1];
                idx[j] = idx[j - 1];
            }
            key[j] = k;
            idx[j] = x;
        }
    }
    for (int64_t width = 16; width < n; width *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * width) {
            int64_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            int64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int right = key[j] < key[i];
                kt[k] = right ? key[j] : key[i];
                it[k++] = right ? idx[j++] : idx[i++];
            }
            while (i < mid) kt[k] = key[i], it[k++] = idx[i++];
            while (j < hi) kt[k] = key[j], it[k++] = idx[j++];
        }
        memcpy(key, kt, sizeof(uint64_t) * (size_t)n);
        memcpy(idx, it, sizeof(int64_t) * (size_t)n);
    }
}

typedef struct {
    unsigned char *rows;  /* (C-contiguous, sorted in place unless args) */
    int64_t *out;         /* argsort: the positions */
    int dtype;
    int64_t n;
} SortJob;

static void sort_rows(void *ctx, int64_t from, int64_t to) {
    SortJob *j = (SortJob *)ctx;
    int64_t n = j->n;
    int z = ITEMSIZE[j->dtype];
    uint64_t *key = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(2 * n + 1));
    int64_t *idx = (int64_t *)malloc(sizeof(int64_t) * (size_t)(2 * n + 1));
    unsigned char *copy = (unsigned char *)malloc((size_t)(n * z + 1));
    if (!key || !idx || !copy) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t r = from; r < to; r++) {
        unsigned char *p = j->rows + r * n * z;
        for (int64_t i = 0; i < n; i++) {
            key[i] = sort_key(p + i * z, j->dtype);
            idx[i] = i;
        }
        if (n <= 1024) short_sort(key, idx, n, key + n, idx + n);
        else radix_sort(key, idx, n, key + n, idx + n);
        if (j->out) {
            memcpy(j->out + r * n, idx, sizeof(int64_t) * (size_t)n);
        } else if (j->dtype == ARR_F64) {
            for (int64_t i = 0; i < n; i++) ((double *)p)[i] = key_value(key[i]);
        } else {  /* (other dtypes: by position) */
            memcpy(copy, p, (size_t)(n * z));
            for (int64_t i = 0; i < n; i++) memcpy(p + i * z, copy + idx[i] * z, (size_t)z);
        }
    }
    free(key);
    free(idx);
    free(copy);
}

/* sort(a, axis?) or argsort(a, axis?): along an axis (the last unless given); argsort keeps equal ones in order */
static LubeValue sorted(LubeVM *vm, const char *fname, LubeValue av, int argc, const LubeValue *argv, int args) {
    Array *a = arr_of(av);
    int axis = a->ndim - 1;
    LubeValue err;
    if (argc > 1 && !axis_of(vm, fname, a, argv[1], &axis, &err)) return err;
    if (a->ndim == 0) return lube->error(vm, "%s() needs an Array of at least 1 dimension", fname);
    Array *rows;
    int64_t n;
    LubeValue rv = axis_last(vm, a, axis, -1, &rows, &n);
    Array *out = rows;
    LubeValue ov = args ? arr_new(vm, ARR_I64, rows->ndim, rows->shape, &out, 0) : lube->retain(rv);
    SortJob j = {rows->data, args ? (int64_t *)out->data : NULL, rows->dtype, n};
    int64_t nrows = n ? rows->size / n : 0;
    par_for(nrows, PARALLEL_WORK / (n ? n : 1) + 1, sort_rows, &j);
    lube->release(rv);
    return axis_back(vm, ov, axis);
}

FN(f_sort) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = sorted(vm, FNAME, keep, argc, argv, 0);
    lube->release(keep);
    return r;
}

FN(f_argsort) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = sorted(vm, FNAME, keep, argc, argv, 1);
    lube->release(keep);
    return r;
}

FN(m_sort) { return sorted(vm, "sort", argv[0], argc, argv, 0); }
FN(m_argsort) { return sorted(vm, "argsort", argv[0], argc, argv, 1); }

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define SWAP_D(i, j) do { double t_ = x[i]; x[i] = x[j]; x[j] = t_; } while (0)

/* Floyd and Rivest's selection of the k-th smallest within x[left..right]: a pivot chosen from a sample around
 * where the k-th should be (selected the same way first), so that each pass leaves little; a slice that will not
 * shrink in `depth` passes is sorted instead. */
static void fr_select(double *x, int64_t left, int64_t right, int64_t k, int depth) {
    while (right > left) {
        if (depth-- <= 0) {  /* LCOV_EXCL_START: (only inputs made to defeat the sample get here) */
            qsort(x + left, (size_t)(right - left + 1), sizeof(double), cmp_double);
            return;
        }  /* LCOV_EXCL_STOP */
        if (right - left > 600) {
            double n = (double)(right - left + 1), i = (double)(k - left + 1), z = log(n), s = 0.5 * exp(2 * z / 3);
            double sd = 0.5 * sqrt(z * s * (n - s) / n) * (i < n / 2 ? -1 : 1);
            double nl = (double)k - i * s / n + sd, nr = (double)k + (n - i) * s / n + sd;
            fr_select(x, nl > (double)left ? (int64_t)nl : left, nr < (double)right ? (int64_t)nr : right, k, depth);
        }
        double t = x[k];
        int64_t i = left, j = right;
        SWAP_D(left, k);
        if (x[right] > t) SWAP_D(right, left);
        while (i < j) {
            SWAP_D(i, j);
            i++;
            j--;
            while (x[i] < t) i++;
            while (x[j] > t) j--;
        }
        if (x[left] == t) {
            SWAP_D(left, j);
        } else {
            j++;
            SWAP_D(j, right);
        }
        if (j <= k) left = j + 1;  /* (left of j: no larger than t; right of it: no smaller) */
        if (k <= j) right = j - 1;
    }
}

/* Puts the k-th smallest of x[0..n) (no NaN among them) at x[k], those before it no larger and those after no
 * smaller. */
static void select_kth(double *x, int64_t n, int64_t k) {
    if (k == 0 || k == n - 1) {  /* (the smallest or the largest: one pass) */
        int64_t m = 0;
        for (int64_t i = 1; i < n; i++)
            if (k ? x[i] > x[m] : x[i] < x[m]) m = i;
        SWAP_D(k, m);
        return;
    }
    int depth = 16;
    for (int64_t len = n; len > 1; len >>= 1) depth += 2;
    fr_select(x, 0, n - 1, k, depth);
}
#undef SWAP_D

/* The quantiles of rows of n: each row copied, and the elements each q needs selected from it. */
typedef struct {
    const double *d;
    double *o, *qs;      /* the q's, as fractions */
    int64_t n, nrows, nq;
    int64_t *pos;        /* the ranks wanted (each q's two neighbors), ascending, npos of them */
    int64_t npos;
} QuantJob;

/* Selects each rank of pos (ascending, np of them, all in lo..hi-1) into place within x[lo..hi): the middle one
 * first, then those before it within the part before it, and those after within the part after. */
static void select_many(double *x, int64_t lo, int64_t hi, const int64_t *pos, int64_t np) {
    if (np == 0) return;
    int64_t m = np / 2, k = pos[m];
    select_kth(x + lo, hi - lo, k - lo);
    select_many(x, lo, k, pos, m);
    select_many(x, k + 1, hi, pos + m + 1, np - m - 1);
}

static void quantile_rows(void *ctx, int64_t from, int64_t to) {
    QuantJob *j = (QuantJob *)ctx;
    int64_t n = j->n;
    double *x = (double *)malloc(sizeof(double) * (size_t)n);
    if (!x) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t r = from; r < to; r++) {
        const double *row = j->d + r * n;
        int nan = 0;
        for (int64_t i = 0; i < n; i++) {
            x[i] = row[i];
            nan |= row[i] != row[i];
        }
        if (!nan) select_many(x, 0, n, j->pos, j->npos);
        for (int64_t k = 0; k < j->nq; k++) {
            double pos = j->qs[k] * (double)(n - 1);
            int64_t lo = (int64_t)floor(pos), hi = lo + 1 < n ? lo + 1 : lo;
            double frac = pos - (double)lo;
            j->o[k * j->nrows + r] = nan ? NAN : x[lo] + (x[hi] - x[lo]) * frac;
        }
    }
    free(x);
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* percentile(a, q, axis?) (q in 0..100) and quantile (q in 0..1): linear between the two nearest, as numpy's
 * default; q a Number or several (their axis first). median(a, axis?) is percentile 50. */
static LubeValue quantiles(LubeVM *vm, const char *fname, LubeValue av, LubeValue qv, double scale, int argc,
                           const LubeValue *argv, int axis_at) {
    Array *a = arr_of(av);
    int axis = ARR_ALL_AXES;
    LubeValue err;
    if (argc > axis_at && !axis_of(vm, fname, a, argv[axis_at], &axis, &err)) return err;
    LubeValue kq;
    Array *q = array_arg(vm, fname, qv, 1, &kq);
    if (!q) return kq;
    for (int64_t i = 0; i < q->size; i++) {
        double x = load(elem_at(q, i), q->dtype) / scale;
        if (!(x >= 0 && x <= 1)) {
            lube->release(kq);
            return lube->error(vm, "%s(): q must be in 0..%g", fname, scale);
        }
    }
    LubeValue flatv = lube->retain(av);
    if (axis == ARR_ALL_AXES) {
        int64_t n = a->size;
        lube->release(flatv);
        flatv = reshaped(vm, av, 1, &n);
        axis = 0;
    }
    Array *flat = arr_of(flatv), *rows;
    int64_t n;
    LubeValue rv = axis_last(vm, flat, axis, ARR_F64, &rows, &n);
    if (n == 0) {
        lube->release(rv);
        lube->release(flatv);
        lube->release(kq);
        return lube->error(vm, "%s() of an empty Array", fname);
    }
    int64_t nrows = rows->size / n;
    int64_t shape[2 * ARR_MAXDIM];
    int nd = 0;
    for (int d = 0; d < q->ndim; d++) shape[nd++] = q->shape[d];
    for (int d = 0; d < rows->ndim - 1; d++) shape[nd++] = rows->shape[d];
    Array *dst;
    LubeValue r = nd > ARR_MAXDIM ? lube->error(vm, "%s(): too many dimensions", fname) : arr_new(vm, ARR_F64, nd, shape, &dst, 0);
    if (nd <= ARR_MAXDIM) {
        int64_t nq = q->size;
        double *qs = (double *)malloc(sizeof(double) * (size_t)(nq + 1));
        int64_t *pos = (int64_t *)malloc(sizeof(int64_t) * (size_t)(2 * nq + 1)), npos = 0;
        if (!qs || !pos) abort();  /* LCOV_EXCL_LINE: out of memory */
        for (int64_t k = 0; k < nq; k++) {
            qs[k] = load(elem_at(q, k), q->dtype) / scale;
            int64_t lo = (int64_t)floor(qs[k] * (double)(n - 1));
            pos[npos++] = lo;
            if (lo + 1 < n) pos[npos++] = lo + 1;
        }
        qsort(pos, (size_t)npos, sizeof(int64_t), cmp_i64);
        int64_t uniq = 0;
        for (int64_t i = 0; i < npos; i++)
            if (!uniq || pos[i] != pos[uniq - 1]) pos[uniq++] = pos[i];
        QuantJob j = {(const double *)rows->data, (double *)dst->data, qs, n, nrows, nq, pos, uniq};
        par_for(nrows, PARALLEL_WORK / n + 1, quantile_rows, &j);
        free(qs);
        free(pos);
    }
    lube->release(rv);
    lube->release(flatv);
    lube->release(kq);
    return scalar_out(r);
}

FN(f_percentile) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = quantiles(vm, FNAME, keep, argv[1], 100, argc, argv, 2);
    lube->release(keep);
    return r;
}

FN(f_quantile) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = quantiles(vm, FNAME, keep, argv[1], 1, argc, argv, 2);
    lube->release(keep);
    return r;
}

FN(f_median) {
    ARRAY_ARG(a, keep, 0);
    (void)a;
    LubeValue r = quantiles(vm, FNAME, keep, lube->number_value(50), 100, argc, argv, 1);
    lube->release(keep);
    return r;
}

/* unique(a, {return_index, return_inverse, return_counts}): the sorted distinct values; with options, a list
 * [values, index?, inverse?, counts?] as numpy gives */
FN(f_unique) {
    ARRAY_ARG(a, keep, 0);
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing();
    int want_index = opt_bool(vm, opts, "return_index", 0), want_inverse = opt_bool(vm, opts, "return_inverse", 0),
        want_counts = opt_bool(vm, opts, "return_counts", 0);
    int64_t n = a->size;
    int z = ITEMSIZE[a->dtype];
    Array *flat = a;  /* (C-contiguous: read without working out each position) */
    LubeValue kflat = is_contiguous(a) ? lube->retain(keep) : arr_copy(vm, a, -1, &flat);
    uint64_t *key = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(2 * n + 1));
    int64_t *idx = (int64_t *)malloc(sizeof(int64_t) * (size_t)(2 * n + 1)), *tmp = idx + n;
    if (!key || !idx) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t i = 0; i < n; i++) {  /* (sort keys keep the values' order; -0 counts as 0, every NaN as one) */
        key[i] = sort_key(flat->data + i * z, a->dtype);
        if (key[i] == 0x7fffffffffffffffull && IS_FLOAT(a->dtype)) key[i] = 0x8000000000000000ull;
        idx[i] = i;
    }
    radix_sort(key, idx, n, key + n, idx + n);  /* (stable: equal ones in the order they came) */
    int64_t groups = 0;
    for (int64_t i = 0; i < n; i++)  /* tmp[g]: where group g starts in idx */
        if (i == 0 || key[i] != key[i - 1]) tmp[groups++] = i;
    Array *vals, *first = NULL, *inv = NULL, *counts = NULL;
    LubeValue vv = arr_new(vm, a->dtype, 1, &groups, &vals, 0);
    int64_t none = 0;  /* (what is not asked for is not made) */
    LubeValue fv = arr_new(vm, ARR_I64, 1, want_index ? &groups : &none, &first, 0),
              iv = arr_new(vm, ARR_I64, 1, want_inverse ? &n : &none, &inv, 0),
              cv = arr_new(vm, ARR_I64, 1, want_counts ? &groups : &none, &counts, 0);
    for (int64_t g = 0; g < groups; g++) {
        int64_t from = tmp[g], to = g + 1 < groups ? tmp[g + 1] : n;
        memcpy(vals->data + g * z, flat->data + idx[from] * z, (size_t)z);
        if (want_index) ((int64_t *)first->data)[g] = idx[from];
        if (want_counts) ((int64_t *)counts->data)[g] = to - from;
        if (want_inverse)
            for (int64_t i = from; i < to; i++) ((int64_t *)inv->data)[idx[i]] = g;
    }
    free(key);
    free(idx);
    lube->release(kflat);
    lube->release(keep);
    if (!want_index && !want_inverse && !want_counts) {
        lube->release(fv);
        lube->release(iv);
        lube->release(cv);
        return vv;
    }
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, vv);
    if (want_index) lube->list_push(r, fv);
    else lube->release(fv);
    if (want_inverse) lube->list_push(r, iv);
    else lube->release(iv);
    if (want_counts) lube->list_push(r, cv);
    else lube->release(cv);
    return r;
}

/* searchsorted(a, v, {side}): where v would go in the sorted a ("left": before equal ones; "right": after) */
FN(f_searchsorted) {
    ARRAY_ARG(a, keep, 0);
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing();
    const char *side = opt_str(vm, opts, "side", "left");
    int right = !strcmp(side, "right");
    if (a->ndim != 1 || (!right && strcmp(side, "left"))) {
        lube->release(keep);
        return lube->error(vm, "searchsorted() needs a sorted Array of 1 dimension (and side \"left\" or \"right\")");
    }
    LubeValue kv;
    Array *v = array_arg(vm, FNAME, argv[1], 1, &kv);
    if (!v) {
        lube->release(keep);
        return kv;
    }
    Array *dst;
    LubeValue r = arr_new(vm, ARR_I64, v->ndim, v->shape, &dst, 0);
    if (a->dtype == ARR_F64 && is_contiguous(a) && is_contiguous(v)) {  /* (in a row: no positions worked out) */
        const double *A = (const double *)a->data;
        int64_t n = a->shape[0];
        for (int64_t i = 0; i < v->size; i++) {
            double x = load(v->data + i * ITEMSIZE[v->dtype], v->dtype);
            /* (without branches: the half to go on in is picked by a select, which a long search misses no
             * branch on; the same place as the halving below finds) */
            int64_t at = 0;
            if (n > 0) {
                const double *base = A;
                int64_t len = n;
                while (len > 1) {
                    int64_t half = len / 2;
                    int on = right ? !LESS_NAN(x, base[half]) : LESS_NAN(base[half], x);
                    base = on ? base + half : base;
                    len -= half;
                }
                at = (base - A) + (right ? !LESS_NAN(x, *base) : LESS_NAN(*base, x));
            }
            ((int64_t *)dst->data)[i] = at;
        }
        lube->release(kv);
        lube->release(keep);
        return scalar_out(r);
    }
    for (int64_t i = 0; i < v->size; i++) {
        double x = load(elem_at(v, i), v->dtype);
        int64_t lo = 0, hi = a->shape[0];
        while (lo < hi) {
            int64_t mid = lo + (hi - lo) / 2;
            double m = load(a->data + mid * a->strides[0] * ITEMSIZE[a->dtype], a->dtype);
            if (right ? !LESS_NAN(x, m) : LESS_NAN(m, x)) lo = mid + 1;
            else hi = mid;
        }
        ((int64_t *)dst->data)[i] = lo;
    }
    lube->release(kv);
    lube->release(keep);
    return scalar_out(r);
}

/* bincount(x, {weights, minlength}): how many times each whole number 0, 1, 2... is in x (or their weights' sum) */
FN(f_bincount) {
    ARRAY_ARG(x, keep, 0);
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing(), w;
    if (x->ndim != 1) {
        lube->release(keep);
        return lube->error(vm, "bincount() needs an Array of 1 dimension");
    }
    int64_t len = (int64_t)opt_num(vm, opts, "minlength", 0);
    int ints = x->dtype == ARR_I64 && is_contiguous(x);  /* (whole numbers in a row: checked and counted directly) */
    if (ints) {
        const int64_t *X = (const int64_t *)x->data;
        int64_t top = -1, low = 0;
        for (int64_t i = 0; i < x->size; i++) {
            top = X[i] > top ? X[i] : top;
            low = X[i] < low ? X[i] : low;
        }
        ints = low >= 0 && top <= MAX_ELEMENTS;
        if (ints && top + 1 > len) len = top + 1;
    }
    for (int64_t i = 0; i < x->size && !ints; i++) {
        double d = load(elem_at(x, i), x->dtype);
        if (d < 0 || d != floor(d) || d > (double)MAX_ELEMENTS) {
            lube->release(keep);
            return lube->error(vm, "bincount() counts whole numbers, 0 or more");
        }
        if ((int64_t)d + 1 > len) len = (int64_t)d + 1;
    }
    Array *wa = NULL;
    LubeValue kw = lube->retain(lube->nothing());
    if (option(vm, opts, "weights", &w)) {
        lube->release(kw);
        wa = array_arg(vm, FNAME, w, 1, &kw);
        if (!wa || wa->size != x->size) {
            lube->release(keep);
            if (!wa) return kw;
            lube->release(kw);
            return lube->error(vm, "bincount(): weights must be as many as the numbers");
        }
    }
    Array *dst;
    LubeValue r = arr_new(vm, wa ? ARR_F64 : ARR_I64, 1, &len, &dst, 1);
    if (ints && !wa) {
        const int64_t *X = (const int64_t *)x->data;
        int64_t *D = (int64_t *)dst->data;
        for (int64_t i = 0; i < x->size; i++) D[X[i]]++;
        lube->release(kw);
        lube->release(keep);
        return r;
    }
    for (int64_t i = 0; i < x->size; i++) {
        int64_t k = (int64_t)load(elem_at(x, i), x->dtype);
        if (wa) ((double *)dst->data)[k] += load(elem_at(wa, i), wa->dtype);
        else ((int64_t *)dst->data)[k]++;
    }
    lube->release(kw);
    lube->release(keep);
    return r;
}

/* ======================================================================
 * Linear algebra
 * ==================================================================== */

FN(f_matmul) {
    (void)argc;
    return matmul(vm, argv[0], argv[1]);
}

/* dot(a, b): a Number times anything; matmul for Arrays of 1 or 2 dimensions */
static LubeValue dotted(LubeVM *vm, LubeValue x, LubeValue y) {
    Array *a = arr_of(x), *b = arr_of(y);
    if (lube->kind(x) == LUBE_NUMBER || lube->kind(y) == LUBE_NUMBER || (a && !a->ndim) || (b && !b->ndim))
        return bin_op(vm, ARR_MUL, x, y, 0, NULL);
    if ((a && a->ndim > 2) || (b && b->ndim > 2)) return lube->error(vm, "dot() of Arrays of more than 2 dimensions: use matmul");
    return matmul(vm, x, y);
}

FN(f_dot) {
    (void)argc;
    return dotted(vm, argv[0], argv[1]);
}

FN(m_dot) {
    (void)argc;
    return dotted(vm, argv[0], argv[1]);
}

/* inner(a, b): over the last axes: a @ b.T for matrices */
FN(f_inner) {
    (void)argc;
    Array *b = arr_of(argv[1]);
    if (!b || b->ndim < 2) return dotted(vm, argv[0], argv[1]);
    LubeValue bt = transposed(vm, argv[1], NULL), r = matmul(vm, argv[0], bt);
    lube->release(bt);
    return r;
}

/* outer(a, b): every element of a times every element of b (both flattened) */
FN(f_outer) {
    (void)argc;
    LubeValue ka, kb;
    Array *a = array_arg(vm, FNAME, argv[0], 0, &ka);
    if (!a) return ka;
    Array *b = array_arg(vm, FNAME, argv[1], 1, &kb);
    if (!b) {
        lube->release(ka);
        return kb;
    }
    int64_t sa[2] = {a->size, 1}, sb[2] = {1, b->size};
    LubeValue ca = reshaped(vm, ka, 2, sa), cb = reshaped(vm, kb, 2, sb);
    LubeValue r = bin_op(vm, ARR_MUL, ca, cb, 0, NULL);
    lube->release(ca);
    lube->release(cb);
    lube->release(ka);
    lube->release(kb);
    return r;
}

/* trace(a): the sum of the diagonal */
FN(f_trace) {
    (void)argc;
    ARRAY_ARG(a, keep, 0);
    if (a->ndim != 2) {
        lube->release(keep);
        return lube->error(vm, "trace() needs a matrix (an Array of 2 dimensions)");
    }
    double s = 0;
    int64_t n = a->shape[0] < a->shape[1] ? a->shape[0] : a->shape[1];
    for (int64_t i = 0; i < n; i++) s += load(a->data + i * (a->strides[0] + a->strides[1]) * ITEMSIZE[a->dtype], a->dtype);
    lube->release(keep);
    return lube->number_value(s);
}

FN(f_det) {
    (void)argc;
    return determinant(vm, FNAME, argv[0], 0);
}

FN(f_slogdet) {
    (void)argc;
    return determinant(vm, FNAME, argv[0], 1);
}

FN(f_inv) {
    (void)argc;
    return solve_or_inv(vm, FNAME, argv[0], NULL);
}

FN(f_solve) {
    (void)argc;
    return solve_or_inv(vm, FNAME, argv[0], &argv[1]);
}

FN(f_cholesky) {
    (void)argc;
    return cholesky(vm, argv[0]);
}

/* qr(a, {mode}): "reduced" (numpy's default) or "complete" */
FN(f_qr) {
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing();
    const char *mode = opt_str(vm, opts, "mode", "reduced");
    if (strcmp(mode, "reduced") && strcmp(mode, "complete")) return lube->error(vm, "qr(): mode is \"reduced\" or \"complete\"");
    return qr(vm, argv[0], !strcmp(mode, "complete"));
}

/* svd(a, {full_matrices, compute_uv}): [U, S, Vh] (U full unless full_matrices is false), or S alone */
FN(f_svd) {
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing();
    return svd(vm, argv[0], opt_bool(vm, opts, "full_matrices", 1), !opt_bool(vm, opts, "compute_uv", 1));
}

FN(f_eigh) {
    (void)argc;
    return eigh(vm, argv[0], 0);
}

FN(f_eigvalsh) {
    (void)argc;
    return eigh(vm, argv[0], 1);
}

/* lstsq(a, b, {rcond}): [x, residuals, rank, s], x minimizing |a x - b| */
FN(f_lstsq) {
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing();
    return pinv_or_lstsq(vm, argv[0], &argv[1], opt_num(vm, opts, "rcond", -1));
}

/* pinv(a, {rcond}) */
FN(f_pinv) {
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing();
    return pinv_or_lstsq(vm, argv[0], NULL, opt_num(vm, opts, "rcond", -1));
}

/* matrix_rank(a): how many singular values are more than the rounding errors' size */
FN(f_matrix_rank) {
    (void)argc;
    LubeValue s = svd(vm, argv[0], 0, 1);
    Array *a = arr_of(s);
    if (!a) return s;
    LubeValue ka;
    Array *m = array_arg(vm, FNAME, argv[0], 0, &ka);
    double cutoff = (a->size ? load(a->data, a->dtype) : 0) * (double)(m->shape[0] > m->shape[1] ? m->shape[0] : m->shape[1]) * 2.220446049250313e-16;
    int64_t rank = 0;
    for (int64_t i = 0; i < a->size; i++) rank += load(elem_at(a, i), a->dtype) > cutoff;
    lube->release(ka);
    lube->release(s);
    return lube->number_value((double)rank);
}

/* norm(x, ord?, axis?): the 2-norm unless asked (vectors: a Number, inf, -inf, 0, 1, 2; matrices: "fro",
 * "nuc", 1, -1, 2, -2, inf, -inf) */
FN(f_norm) {
    ARRAY_ARG(a, keep, 0);
    LubeValue ord = argc > 1 ? argv[1] : lube->nothing(), err, r;
    int axis = ARR_ALL_AXES;
    if (argc > 2 && !axis_of(vm, FNAME, a, argv[2], &axis, &err)) {
        lube->release(keep);
        return err;
    }
    const char *os = lube->str(ord, NULL);
    int has_ord = lube->kind(ord) == LUBE_NUMBER || os;
    if ((!has_ord || (os && !strcmp(os, "fro") && a->ndim == 2)) && axis == ARR_ALL_AXES && IS_FLOAT(a->dtype) &&
        is_contiguous(a)) {  /* (of every element, in a row: the squares' sum in one pass) */
        r = lube->number_value(sqrt(sum_or_squares(a->data, a->dtype, a->size, 1, 0.0)));
    } else if (!has_ord || (os && !strcmp(os, "fro") && a->ndim == 2 && axis == ARR_ALL_AXES)) {  /* sqrt of the sum of squares */
        LubeValue sq = un_op(vm, ARR_SQUARE, keep), f = arr_from(vm, sq, ARR_F64, &a);
        LubeValue s = reduce_op(vm, ARR_SUM, f, axis, 0, 0);
        r = scalar_out(un_op(vm, ARR_SQRT, s));
        lube->release(sq);
        lube->release(f);
        lube->release(s);
    } else if (a->ndim == 2 && axis == ARR_ALL_AXES) {  /* a matrix norm */
        double p = lube->number(ord);
        if (os && !strcmp(os, "nuc")) {
            LubeValue s = svd(vm, keep, 0, 1);
            r = scalar_out(reduce_op(vm, ARR_SUM, s, ARR_ALL_AXES, 0, 0));
            lube->release(s);
        } else if (!os && (p == 2 || p == -2)) {
            LubeValue s = svd(vm, keep, 0, 1);
            r = scalar_out(reduce_op(vm, p > 0 ? ARR_MAX : ARR_MIN, s, ARR_ALL_AXES, 0, 0));
            lube->release(s);
        } else if (!os && (p == 1 || p == -1 || isinf(p))) {
            LubeValue ab = un_op(vm, ARR_ABS, keep), sums = reduce_op(vm, ARR_SUM, ab, isinf(p) ? 1 : 0, 0, 0);
            r = scalar_out(reduce_op(vm, p > 0 ? ARR_MAX : ARR_MIN, sums, ARR_ALL_AXES, 0, 0));
            lube->release(ab);
            lube->release(sums);
        } else {
            r = lube->error(vm, "norm(): a matrix has no such norm (\"fro\", \"nuc\", 1, -1, 2, -2, inf or -inf)");
        }
    } else if (os) {
        r = lube->error(vm, "norm(): \"%s\" is a matrix norm (for an Array of 2 dimensions)", os);
    } else {  /* a vector norm (along an axis, or of everything) */
        double p = lube->number(ord);
        LubeValue ab = un_op(vm, ARR_ABS, keep), f;
        Array *af;
        f = arr_from(vm, ab, ARR_F64, &af);
        if (isinf(p)) {
            r = scalar_out(reduce_op(vm, p > 0 ? ARR_MAX : ARR_MIN, f, axis, 0, 0));
        } else if (p == 0) {
            LubeValue nz = bin_op(vm, ARR_NE, f, lube->number_value(0), 0, NULL);
            r = scalar_out(reduce_op(vm, ARR_SUM, nz, axis, 0, 0));
            lube->release(nz);
            if (lube->kind(r) == LUBE_NUMBER) {
                double c = lube->number(r);
                r = lube->number_value(c);
            }
        } else {
            LubeValue pw = bin_op(vm, ARR_POW, f, lube->number_value(p), 0, NULL), s = reduce_op(vm, ARR_SUM, pw, axis, 0, 0);
            r = scalar_out(bin_op(vm, ARR_POW, s, lube->number_value(1 / p), 0, NULL));
            lube->release(pw);
            lube->release(s);
        }
        lube->release(ab);
        lube->release(f);
    }
    lube->release(keep);
    return r;
}

/* ======================================================================
 * Random Arrays (numpy's numbers for the same seed)
 * ==================================================================== */

/* seed(n?): from n (a whole number in 0..2^32-1), or from the clock */
FN(f_seed) {
    double d = argc ? lube->number(argv[0]) : 0;
    if (argc && (lube->kind(argv[0]) != LUBE_NUMBER || d != floor(d) || d < 0 || d > 4294967295.0))
        return lube->error(vm, "seed() takes a whole number in 0..4294967295");
    lube->lock_acquire(rng_lock);
    rng_seed(argc ? (uint32_t)d : (uint32_t)time(NULL) ^ (uint32_t)(uint64_t)(lube->now() * 1e6));
    lube->lock_release(rng_lock);
    return lube->nothing();
}

enum { R_UNIFORM, R_NORMAL, R_EXPONENTIAL };

/* An Array of the given shape (nd -1: a Number) of draws: lo + (hi - lo) u, loc + scale z, or scale * e. */
static LubeValue draws(LubeVM *vm, int kind, int nd, const int64_t *shape, double a, double b) {
    Array *r;
    LubeValue v = nd < 0 ? scalar_array(vm, 0, ARR_F64, &r) : arr_new(vm, ARR_F64, nd, shape, &r, 0);
    if (!r) return v;
    double *d = (double *)r->data;
    rng_lock_on();
    for (int64_t i = 0; i < r->size; i++) {
        if (kind == R_UNIFORM) d[i] = a + (b - a) * next_double();
        else if (kind == R_NORMAL) d[i] = a + b * next_gauss();
        else d[i] = a * -log(1.0 - next_double());
    }
    rng_lock_off();
    return nd < 0 ? scalar_out(v) : v;
}

/* A size argument (numpy's size=): a Number or a list; nothing: -1 (one Number). */
static int size_arg(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int at, int64_t *shape, LubeValue *err) {
    if (argc <= at) return -1;
    if (lube->kind(argv[at]) == LUBE_STR) {
        *err = lube->error(vm, "%s(): a size is a Number or a list of them, not a str", fname);
        return -2;
    }
    int nd, dt;
    if (!shape_args(vm, fname, 1, argv + at, 0, &nd, shape, &dt, err)) return -2;
    return nd;
}

static int number_args(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int count, double *out, LubeValue *err) {
    for (int i = 0; i < count && i < argc; i++) {
        if (lube->kind(argv[i]) != LUBE_NUMBER) {
            *err = lube->error(vm, "%s() expects a Number as argument %d, got %s", fname, i + 1, lube->a_type(argv[i]));
            return 0;
        }
        out[i] = lube->number(argv[i]);
    }
    return 1;
}

/* rand(shape...): uniform in [0, 1); randn(shape...): standard normal; no shape: a Number */
static LubeValue rand_call(LubeVM *vm, const char *fname, int kind, int argc, const LubeValue *argv) {
    int nd = -1, dt;
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    if (argc && !shape_args(vm, fname, argc, argv, 0, &nd, shape, &dt, &err)) return err;
    return draws(vm, kind, nd, shape, kind == R_UNIFORM ? 0 : 0, 1);
}

FN(f_rand) { return rand_call(vm, FNAME, R_UNIFORM, argc, argv); }
FN(f_randn) { return rand_call(vm, FNAME, R_NORMAL, argc, argv); }

/* random(size?) and standard_normal(size?) */
FN(f_random) {
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    int nd = size_arg(vm, FNAME, argc, argv, 0, shape, &err);
    return nd < -1 ? err : draws(vm, R_UNIFORM, nd, shape, 0, 1);
}

FN(f_standard_normal) {
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    int nd = size_arg(vm, FNAME, argc, argv, 0, shape, &err);
    return nd < -1 ? err : draws(vm, R_NORMAL, nd, shape, 0, 1);
}

/* uniform(low?, high?, size?), normal(loc?, scale?, size?), exponential(scale?, size?) */
static LubeValue dist_call(LubeVM *vm, const char *fname, int kind, int argc, const LubeValue *argv, int params,
                           double a, double b) {
    double v[2] = {a, b};
    int64_t shape[ARR_MAXDIM];
    LubeValue err;
    int given = argc < params ? argc : params;
    if (!number_args(vm, fname, given, argv, given, v, &err)) return err;
    int nd = size_arg(vm, fname, argc, argv, params, shape, &err);
    if (nd < -1) return err;
    if ((kind == R_NORMAL && v[1] < 0) || (kind == R_EXPONENTIAL && v[0] < 0))
        return lube->error(vm, "%s(): the scale cannot be negative", fname);
    return draws(vm, kind, nd, shape, v[0], v[1]);
}

FN(f_uniform) { return dist_call(vm, FNAME, R_UNIFORM, argc, argv, 2, 0, 1); }
FN(f_normal) { return dist_call(vm, FNAME, R_NORMAL, argc, argv, 2, 0, 1); }
FN(f_exponential) { return dist_call(vm, FNAME, R_EXPONENTIAL, argc, argv, 1, 1, 0); }

/* randint(low, high?, size?): whole numbers in low..high-1 (0..low-1 without high); int64 */
FN(f_randint) {
    double v[2];
    LubeValue err;
    int nums = argc > 1 && lube->kind(argv[1]) == LUBE_NUMBER ? 2 : 1;
    if (!number_args(vm, FNAME, nums, argv, nums, v, &err)) return err;
    double lo = nums == 2 ? v[0] : 0, hi = nums == 2 ? v[1] : v[0];
    if (lo != floor(lo) || hi != floor(hi) || !(hi > lo) || hi - lo > 1.8e19)
        return lube->error(vm, "randint(): low must be less than high, both whole numbers");
    int64_t shape[ARR_MAXDIM];
    int nd = size_arg(vm, FNAME, argc, argv, nums, shape, &err);
    if (nd < -1) return err;
    Array *r;
    LubeValue rv = nd < 0 ? scalar_array(vm, 0, ARR_I64, &r) : arr_new(vm, ARR_I64, nd, shape, &r, 0);
    if (!r) return rv;
    uint64_t range = (uint64_t)(hi - lo - 1);
    rng_lock_on();
    for (int64_t i = 0; i < r->size; i++) ((int64_t *)r->data)[i] = (int64_t)lo + (int64_t)next_below(range);
    rng_lock_off();
    return nd < 0 ? scalar_out(rv) : rv;
}

/* Shuffles n items of size z (rows) in place, as numpy's shuffle does (Fisher-Yates from the end). */
static void shuffle_rows(unsigned char *p, int64_t n, size_t z) {
    unsigned char small[64], *t = z <= sizeof small ? small : (unsigned char *)malloc(z);
    if (!t) abort();  /* LCOV_EXCL_LINE: out of memory */
    rng_lock_on();
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = (int64_t)next_below((uint64_t)i);
        memcpy(t, p + (size_t)i * z, z);
        memcpy(p + (size_t)i * z, p + (size_t)j * z, z);
        memcpy(p + (size_t)j * z, t, z);
    }
    rng_lock_off();
    if (t != small) free(t);
}

/* permutation(x): 0..x-1 shuffled (x a Number), or x's rows shuffled; shuffle(x) is the same (a new Array: a
 * plugin cannot change the caller's variable) */
FN(f_permutation) {
    (void)argc;
    LubeValue r;
    Array *c;
    if (lube->kind(argv[0]) == LUBE_NUMBER) {
        double n = lube->number(argv[0]);
        if (n < 0 || n != floor(n) || n > (double)MAX_ELEMENTS) return lube->error(vm, "permutation() of a whole number, 0 or more");
        int64_t len = (int64_t)n;
        r = arr_new(vm, ARR_I64, 1, &len, &c, 0);
        for (int64_t i = 0; i < len; i++) ((int64_t *)c->data)[i] = i;
    } else {
        LubeValue keep;
        Array *a = array_arg(vm, FNAME, argv[0], 0, &keep);
        if (!a) return keep;
        if (!a->ndim) {
            lube->release(keep);
            return lube->error(vm, "permutation() needs an Array of at least 1 dimension");
        }
        r = arr_copy(vm, a, -1, &c);
        lube->release(keep);
    }
    if (c->ndim && c->shape[0] > 1) shuffle_rows(c->data, c->shape[0], (size_t)(c->size / c->shape[0]) * (size_t)ITEMSIZE[c->dtype]);
    return r;
}

/* choice(a, size?, {replace, p}): picks from a (or from 0..a-1, a a Number), as numpy's choice */
FN(f_choice) {
    LubeValue keep = lube->retain(lube->nothing()), opts = lube->nothing(), err;
    int64_t pop;
    Array *a = NULL;
    if (lube->kind(argv[0]) == LUBE_NUMBER) {
        double n = lube->number(argv[0]);
        if (n < 0 || n != floor(n)) return lube->error(vm, "choice() picks from 0..n-1, n a whole number");
        pop = (int64_t)n;
    } else {
        lube->release(keep);
        a = array_arg(vm, FNAME, argv[0], 0, &keep);
        if (!a) return keep;
        if (a->ndim != 1) {
            lube->release(keep);
            return lube->error(vm, "choice() picks from an Array of 1 dimension");
        }
        pop = a->size;
    }
    int at = 1;
    int64_t shape[ARR_MAXDIM];
    int nd = -1;
    if (argc > at && !is_object(argv[at])) {
        if ((nd = size_arg(vm, FNAME, argc, argv, at, shape, &err)) < -1) {
            lube->release(keep);
            return err;
        }
        at++;
    }
    if (argc > at) opts = argv[at];
    int replace = opt_bool(vm, opts, "replace", 1);
    LubeValue pv, kp = lube->retain(lube->nothing());
    Array *p = NULL;
    if (option(vm, opts, "p", &pv)) {
        lube->release(kp);
        p = array_arg(vm, FNAME, pv, 1, &kp);
        double total = 0;
        for (int64_t i = 0; p && i < p->size; i++) total += load(elem_at(p, i), p->dtype);
        if (!p || p->ndim != 1 || p->size != pop || fabs(total - 1) > 1e-8) {
            lube->release(keep);
            if (!p) return kp;
            lube->release(kp);
            return lube->error(vm, "choice(): p must be %.0f probabilities that add up to 1", (double)pop);
        }
    }
    int64_t count = nd < 0 ? 1 : shape_size(nd, shape);
    if (pop == 0 || (!replace && count > pop)) {
        lube->release(keep);
        lube->release(kp);
        return lube->error(vm, pop == 0 ? "choice() from nothing" : "choice(): cannot take more than there are without replacing");
    }
    int64_t *picks = (int64_t *)malloc(sizeof(int64_t) * (size_t)(count + pop + 1));
    double *cdf = (double *)malloc(sizeof(double) * (size_t)(pop + 1));
    if (!picks || !cdf) abort();  /* LCOV_EXCL_LINE: out of memory */
    if (p) {
        double *w = (double *)malloc(sizeof(double) * (size_t)pop);
        if (!w) abort();  /* LCOV_EXCL_LINE: out of memory */
        for (int64_t i = 0; i < pop; i++) w[i] = load(elem_at(p, i), p->dtype);
        int64_t found = 0;
        rng_lock_on();
        while (found < count) {  /* (as numpy: draw, then drop what was found, until there are enough) */
            int64_t want = replace ? count : count - found;
            double total = 0;
            for (int64_t i = 0; i < pop; i++) cdf[i] = total += w[i];
            for (int64_t i = 0; i < pop; i++) cdf[i] /= total;
            int64_t *fresh = picks + count, nfresh = 0;
            double *u = (double *)malloc(sizeof(double) * (size_t)want);
            if (!u) abort();  /* LCOV_EXCL_LINE: out of memory */
            for (int64_t k = 0; k < want; k++) u[k] = next_double();
            for (int64_t k = 0; k < want; k++) {
                int64_t lo = 0, hi = pop;  /* searchsorted, side right */
                while (lo < hi) {
                    int64_t mid = (lo + hi) / 2;
                    if (cdf[mid] <= u[k]) lo = mid + 1;
                    else hi = mid;
                }
                if (replace) {
                    picks[found++] = lo;
                    continue;
                }
                int seen = 0;
                for (int64_t j = 0; j < nfresh && !seen; j++) seen = fresh[j] == lo;
                if (!seen) fresh[nfresh++] = lo;
            }
            free(u);
            if (!replace) {  /* the new ones in the order numpy's unique() leaves their first places in */
                for (int64_t j = 0; j < nfresh && found < count; j++) {
                    picks[found++] = fresh[j];
                    w[fresh[j]] = 0;
                }
            }
        }
        rng_lock_off();
        free(w);
    } else if (replace) {
        rng_lock_on();
        for (int64_t k = 0; k < count; k++) picks[k] = (int64_t)next_below((uint64_t)(pop - 1));
        rng_lock_off();
    } else {
        int64_t *perm = picks + count;
        for (int64_t i = 0; i < pop; i++) perm[i] = i;
        shuffle_rows((unsigned char *)perm, pop, sizeof(int64_t));
        memcpy(picks, perm, sizeof(int64_t) * (size_t)count);
    }
    Array *r;
    int dtype = a ? a->dtype : ARR_I64;
    LubeValue rv = nd < 0 ? scalar_array(vm, 0, dtype, &r) : arr_new(vm, dtype, nd, shape, &r, 0);
    for (int64_t k = 0; k < count; k++) {
        if (a) memcpy(r->data + k * ITEMSIZE[dtype], elem_at(a, picks[k]), (size_t)ITEMSIZE[dtype]);
        else ((int64_t *)r->data)[k] = picks[k];
    }
    free(picks);
    free(cdf);
    lube->release(keep);
    lube->release(kp);
    return nd < 0 ? scalar_out(rv) : rv;
}

/* ======================================================================
 * Files
 * ==================================================================== */

static int path_arg(LubeVM *vm, const char *fname, LubeValue v, const char **out, LubeValue *err) {
    *out = lube->str(v, NULL);
    if (!*out) *err = lube->error(vm, "%s() expects a path (a str) as argument 1, got %s", fname, lube->a_type(v));
    return *out != NULL;
}

FN(f_save) {
    (void)argc;
    const char *path;
    LubeValue err;
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    LubeValue keep;
    Array *a = array_arg(vm, FNAME, argv[1], 1, &keep);
    if (!a) return keep;
    LubeValue r = npy_save(vm, path, keep);
    lube->release(keep);
    return r;
}

FN(f_load) {
    (void)argc;
    const char *path;
    LubeValue err;
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    return npy_load(vm, path);
}

/* loadtxt(path, {delimiter, skiprows, dtype}) */
FN(f_loadtxt) {
    const char *path;
    LubeValue err, opts = argc > 1 ? argv[1] : lube->nothing(), dv;
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    int dtype = ARR_F64;
    if (option(vm, opts, "dtype", &dv) && (dtype = dtype_arg(vm, FNAME, dv, &err)) < -1) return err;
    return txt_load(vm, path, opt_str(vm, opts, "delimiter", NULL), (int64_t)opt_num(vm, opts, "skiprows", 0), dtype);
}

/* savetxt(path, a, {fmt, delimiter, header}) */
FN(f_savetxt) {
    const char *path;
    LubeValue err, opts = argc > 2 ? argv[2] : lube->nothing();
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    LubeValue keep;
    Array *a = array_arg(vm, FNAME, argv[1], 1, &keep);
    if (!a) return keep;
    LubeValue r = txt_save(vm, path, keep, opt_str(vm, opts, "fmt", "%.18e"), opt_str(vm, opts, "delimiter", " "),
                           opt_str(vm, opts, "header", NULL));
    lube->release(keep);
    return r;
}

FN(f_load_safetensors) {
    (void)argc;
    const char *path;
    LubeValue err;
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    return st_load(vm, path);
}

/* save_safetensors(path, arrays): arrays an object of Arrays by name */
static LubeValue save_st(LubeVM *vm, const char *path, LubeValue obj) {
    if (lube->kind(obj) != LUBE_OBJECT) return lube->error(vm, "save_safetensors() expects an object of Arrays by name, got %s", lube->a_type(obj));
    LubeValue keys = lube->object_keys(vm, obj);
    int n = (int)lube->list_len(keys);
    const char **names = (const char **)malloc(sizeof(char *) * (size_t)(n + 1));
    Array **arrays = (Array **)malloc(sizeof(Array *) * (size_t)(n + 1));
    if (!names || !arrays) abort();  /* LCOV_EXCL_LINE: out of memory */
    LubeValue r = lube->nothing();
    for (int i = 0; i < n; i++) {
        LubeValue k = lube->list_get(keys, (size_t)i), v;
        names[i] = lube->str(k, NULL);
        lube->object_get(vm, obj, names[i] ? names[i] : "", &v);
        arrays[i] = names[i] ? arr_of(v) : NULL;
        if (!arrays[i]) {
            r = lube->error(vm, "save_safetensors(): every value must be an Array, named by a str");
            break;
        }
    }
    if (!lube->failed(r)) r = st_save(vm, path, obj, n, names, arrays);
    free(names);
    free(arrays);
    lube->release(keys);
    return r;
}

FN(f_save_safetensors) {
    (void)argc;
    const char *path;
    LubeValue err;
    if (!path_arg(vm, FNAME, argv[0], &path, &err)) return err;
    return save_st(vm, path, argv[1]);
}

static LubeValue export_load_st(LubeVM *vm, const char *path) { return st_load(vm, path); }

static void export_seed(uint32_t n) {
    lube->lock_acquire(rng_lock);
    rng_seed(n);
    lube->lock_release(rng_lock);
}

static int export_describe(const Array *a, char *buf, size_t size) { return arr_describe(a, buf, size); }
static LubeValue export_save_st(LubeVM *vm, const char *path, LubeValue obj) { return save_st(vm, path, obj); }

/* ======================================================================
 * The machinery: BLAS and threads
 * ==================================================================== */

/* blas(): which BLAS products use ("Accelerate", "OpenBLAS"...), or "none" (our own kernel) */
FN(f_blas) {
    (void)argc;
    (void)argv;
    if (!blas_tried) blas_open();
    const char *name = blas_off || !blas_dgemm ? "none" : blas_name;
    return lube->new_str(vm, name, strlen(name));
}

/* use_blas(on): products on the system's BLAS, or on our own kernel */
FN(f_use_blas) {
    (void)argc;
    if (lube->kind(argv[0]) != LUBE_BOOL) return lube->error(vm, "use_blas() takes true or false");
    lube->lock_acquire(blas_lock);
    blas_off = !lube->truthy(argv[0]);
    lube->lock_release(blas_lock);
    return lube->nothing();
}

/* threads(n?): how many threads big loops use (n 0: one per core, as at first); gives the number */
FN(f_threads) {
    if (argc) {
        double n = lube->number(argv[0]);
        if (lube->kind(argv[0]) != LUBE_NUMBER || n < 0 || n != floor(n) || n > MAX_THREADS)
            return lube->error(vm, "threads() takes a whole number in 0..%d (0: one per core)", MAX_THREADS);
        STORE(thread_limit, (int)n);
    }
    return lube->number_value(threads_now());
}

/* ======================================================================
 * Methods and properties
 * ==================================================================== */

FN(m_astype) {
    (void)argc;
    LubeValue err;
    int dtype = dtype_arg(vm, "astype", argv[1], &err);
    if (dtype < -1) return err;
    return arr_copy(vm, arr_of(argv[0]), dtype, NULL);
}

FN(m_copy) {
    (void)argc;
    return arr_copy(vm, arr_of(argv[0]), -1, NULL);
}

FN(m_to_list) {
    (void)argc;
    Array *a = arr_of(argv[0]);
    return to_list(vm, a, 0, a->data);
}

FN(m_to_bytes) {
    (void)argc;
    Array *a;
    LubeValue keep = as_contiguous(vm, argv[0], -1, &a);
    LubeValue r = lube->new_bytes(vm, a->data, (size_t)a->size * (size_t)ITEMSIZE[a->dtype]);
    lube->release(keep);
    return r;
}

/* item(i?): the one element (or the element at C-order position i) as a Number */
FN(m_item) {
    Array *a = arr_of(argv[0]);
    if (argc > 1) {
        double d = lube->number(argv[1]);
        if (lube->kind(argv[1]) != LUBE_NUMBER || d != floor(d) || d < -(double)a->size || d >= (double)a->size)
            return lube->error(vm, "item(): no position %g in an Array of %.0f elements", d, (double)a->size);
        return elem_value(elem_at(a, d < 0 ? (int64_t)d + a->size : (int64_t)d), a->dtype);
    }
    if (a->size != 1) return lube->error(vm, "item() of an Array of %.0f elements: give a position", (double)a->size);
    return elem_value(a->data, a->dtype);
}

FN(m_ravel) {
    (void)argc;
    int64_t n = arr_of(argv[0])->size;
    return reshaped(vm, argv[0], 1, &n);
}

FN(p_shape) {
    (void)argc;
    Array *a = arr_of(argv[0]);
    LubeValue l = lube->new_list(vm);
    for (int d = 0; d < a->ndim; d++) lube->list_push(l, lube->number_value((double)a->shape[d]));
    return l;
}

FN(p_ndim) {
    (void)vm;
    (void)argc;
    return lube->number_value(arr_of(argv[0])->ndim);
}

FN(p_size) {
    (void)vm;
    (void)argc;
    return lube->number_value((double)arr_of(argv[0])->size);
}

FN(p_dtype) {
    (void)argc;
    const char *n = DTYPE_NAMES[arr_of(argv[0])->dtype];
    return lube->new_str(vm, n, strlen(n));
}

FN(p_T) {
    (void)argc;
    return transposed(vm, argv[0], NULL);
}

FN(p_itemsize) {
    (void)vm;
    (void)argc;
    return lube->number_value(ITEMSIZE[arr_of(argv[0])->dtype]);
}

FN(p_nbytes) {
    (void)vm;
    (void)argc;
    Array *a = arr_of(argv[0]);
    return lube->number_value((double)a->size * ITEMSIZE[a->dtype]);
}

/* ======================================================================
 * Operators
 * ==================================================================== */

#define OPERATOR(name, op)                                                                                     \
    FN(o_##name) {                                                                                             \
        (void)argc;                                                                                            \
        return bin_op(vm, op, argv[0], argv[1], 0, NULL);                                                      \
    }
OPERATOR(add, ARR_ADD)
OPERATOR(sub, ARR_SUB)
OPERATOR(mul, ARR_MUL)
OPERATOR(div, ARR_DIV)
OPERATOR(mod, ARR_MOD)
OPERATOR(pow, ARR_POW)
OPERATOR(eq, ARR_EQ)
OPERATOR(ne, ARR_NE)
OPERATOR(lt, ARR_LT)
OPERATOR(le, ARR_LE)
OPERATOR(gt, ARR_GT)
OPERATOR(ge, ARR_GE)

#define INPLACE(name, op)                                                                                      \
    FN(i_##name) {                                                                                             \
        (void)argc;                                                                                            \
        int done;                                                                                              \
        return bin_op(vm, op, argv[0], argv[1], 1, &done);                                                     \
    }
INPLACE(add, ARR_ADD)
INPLACE(sub, ARR_SUB)
INPLACE(mul, ARR_MUL)
INPLACE(div, ARR_DIV)
INPLACE(mod, ARR_MOD)

FN(o_matmul) {
    (void)argc;
    return matmul(vm, argv[0], argv[1]);
}

FN(o_neg) {
    (void)argc;
    return un_op(vm, ARR_NEG, argv[0]);
}

FN(o_get) { return index_get(vm, argc, argv); }
FN(o_set) { return index_set(vm, argc, argv); }

FN(o_len) {
    (void)argc;
    Array *a = arr_of(argv[0]);
    if (!a->ndim) return lube->error(vm, "len() of a 0-dimensional Array (it has one element: item())");
    return lube->number_value((double)a->shape[0]);
}

/* A copy that changes apart: the same memory until one of them changes (arr_own). */
FN(o_copy) {
    (void)argc;
    Array *v;
    return arr_view(vm, arr_of(argv[0]), &v);
}

/* ======================================================================
 * For other plugins (array.h)
 * ==================================================================== */

static Array *export_get(LubeValue v) { return arr_of(v); }

static LubeValue export_make(LubeVM *vm, int dtype, int nd, const int64_t *shape, void **data) {
    Array *a;
    LubeValue v = arr_new(vm, dtype, nd, shape, &a, 1);
    if (data) *data = a ? a->data : NULL;
    return v;
}

static LubeValue export_make_empty(LubeVM *vm, int dtype, int nd, const int64_t *shape, void **data) {
    Array *a;
    LubeValue v = arr_new(vm, dtype, nd, shape, &a, 0);
    if (data) *data = a ? a->data : NULL;
    return v;
}

static LubeValue export_from(LubeVM *vm, LubeValue x, int dtype) {
    Array *a;
    return arr_from(vm, x, dtype, &a);
}

static LubeValue export_contiguous(LubeVM *vm, LubeValue x, int dtype) {
    Array *a;
    return as_contiguous(vm, x, dtype, &a);
}

static LubeValue export_binary(LubeVM *vm, int op, LubeValue a, LubeValue b) { return bin_op(vm, op, a, b, 0, NULL); }

static void export_own(Array *a) {
    arr_own(a);
    if (!is_contiguous(a)) {  /* (its own, but a view: made contiguous) */
        a->buf->refs++;
        arr_own(a);
    }
}

static void export_fill_random(void *data, int dtype, int64_t n, int normal, double a, double b) {
    rng_lock_on();
    for (int64_t i = 0; i < n; i++)
        store((unsigned char *)data + i * ITEMSIZE[dtype], dtype, normal ? a + b * next_gauss() : a + (b - a) * next_double());
    rng_lock_off();
}
static LubeValue export_reduce(LubeVM *vm, int op, LubeValue a, int axis, int keepdims) {
    return reduce_op(vm, op, a, axis, keepdims, 0);
}

static const ArrayExports EXPORTS = {
    ARRAY_EXPORTS, NULL, DTYPE_NAMES, ITEMSIZE,
    export_get, export_make, export_from, export_contiguous, export_binary, un_op, matmul, export_reduce,
    export_reshape, transposed, sum_to, index_get, par_for, export_own, export_fill_random,
    export_load_st, export_save_st, export_seed, export_describe, export_make_empty,
};
static ArrayExports exports;

LUBE_PLUGIN(array) {
    lube = api;
    if (api->version < 2) return -1;
    pool_lock = lube->lock_new();
    blas_lock = lube->lock_new();
    rng_lock = lube->lock_new();
    core_count = cores();
    ArrayType = api->type(plugin, "Array", sizeof(Array), arr_release, arr_describe);
    exports = EXPORTS;
    exports.type = ArrayType;
    api->provide(plugin, &exports);
    api->constructor(ArrayType, f_array, 1, 2);
    struct { const char *name; LubeFn fn; int min, max; } fns[] = {
        {"array", f_array, 1, 2}, {"asarray", f_asarray, 1, 2}, {"zeros", f_zeros, 0, -1}, {"ones", f_ones, 0, -1},
        {"empty", f_zeros, 0, -1}, {"full", f_full, 2, 3}, {"zeros_like", f_zeros_like, 1, 2},
        {"ones_like", f_ones_like, 1, 2}, {"empty_like", f_zeros_like, 1, 2}, {"full_like", f_full_like, 2, 3},
        {"arange", f_arange, 1, 4}, {"linspace", f_linspace, 2, 4}, {"logspace", f_logspace, 2, 4},
        {"eye", f_eye, 1, 4}, {"identity", f_identity, 1, 2}, {"diag", f_diag, 1, 2}, {"from_bytes", f_from_bytes, 1, 3},
        {"reshape", f_reshape, 2, -1}, {"ravel", f_ravel, 1, 1}, {"transpose", f_transpose, 1, -1},
        {"swapaxes", f_swapaxes, 3, 3}, {"expand_dims", f_expand_dims, 2, 2}, {"squeeze", f_squeeze, 1, 2},
        {"broadcast_to", f_broadcast_to, 2, -1}, {"flip", f_flip, 1, 2}, {"concatenate", f_concatenate, 1, 2},
        {"stack", f_stack, 1, 2}, {"vstack", f_vstack, 1, 1}, {"hstack", f_hstack, 1, 1}, {"tile", f_tile, 2, 2},
        {"repeat", f_repeat, 2, 3}, {"roll", f_roll, 2, 3},
        {"negative", f_negative, 1, 1}, {"abs", f_abs, 1, 1}, {"absolute", f_abs, 1, 1}, {"sqrt", f_sqrt, 1, 1},
        {"exp", f_exp, 1, 1}, {"log", f_log, 1, 2}, {"log2", f_log2, 1, 1}, {"log10", f_log10, 1, 1},
        {"log1p", f_log1p, 1, 1}, {"expm1", f_expm1, 1, 1}, {"sin", f_sin, 1, 1}, {"cos", f_cos, 1, 1},
        {"tan", f_tan, 1, 1}, {"arcsin", f_arcsin, 1, 1}, {"arccos", f_arccos, 1, 1}, {"arctan", f_arctan, 1, 1},
        {"sinh", f_sinh, 1, 1}, {"cosh", f_cosh, 1, 1}, {"tanh", f_tanh, 1, 1}, {"arcsinh", f_arcsinh, 1, 1},
        {"arccosh", f_arccosh, 1, 1}, {"arctanh", f_arctanh, 1, 1}, {"floor", f_floor, 1, 1}, {"ceil", f_ceil, 1, 1},
        {"trunc", f_trunc, 1, 1}, {"rint", f_rint, 1, 1}, {"sign", f_sign, 1, 1}, {"square", f_square, 1, 1},
        {"reciprocal", f_reciprocal, 1, 1}, {"cbrt", f_cbrt, 1, 1}, {"exp2", f_exp2, 1, 1}, {"erf", f_erf, 1, 1},
        {"isnan", f_isnan, 1, 1}, {"isinf", f_isinf, 1, 1}, {"isfinite", f_isfinite, 1, 1},
        {"logical_not", f_logical_not, 1, 1}, {"round", f_round, 1, 2}, {"clip", f_clip, 3, 3},
        {"add", f_add, 2, 2}, {"subtract", f_subtract, 2, 2}, {"multiply", f_multiply, 2, 2},
        {"divide", f_divide, 2, 2}, {"mod", f_mod, 2, 2}, {"remainder", f_mod, 2, 2}, {"power", f_power, 2, 2},
        {"pow", f_power, 2, 2}, {"maximum", f_maximum, 2, 2}, {"minimum", f_minimum, 2, 2},
        {"arctan2", f_arctan2, 2, 2}, {"atan2", f_arctan2, 2, 2}, {"hypot", f_hypot, 2, 2}, {"fmod", f_fmod, 2, 2},
        {"floor_divide", f_floor_divide, 2, 2}, {"copysign", f_copysign, 2, 2}, {"less", f_less, 2, 2},
        {"less_equal", f_less_equal, 2, 2}, {"greater", f_greater, 2, 2}, {"greater_equal", f_greater_equal, 2, 2},
        {"equal", f_equal, 2, 2}, {"not_equal", f_not_equal, 2, 2}, {"logical_and", f_logical_and, 2, 2},
        {"logical_or", f_logical_or, 2, 2}, {"logical_xor", f_logical_xor, 2, 2}, {"where", f_where, 1, 3},
        {"nonzero", f_nonzero, 1, 1}, {"isclose", f_isclose, 2, 3}, {"allclose", f_allclose, 2, 3},
        {"array_equal", f_array_equal, 2, 2},
        {"sum", f_sum, 1, 3}, {"prod", f_prod, 1, 3}, {"mean", f_mean, 1, 3}, {"std", f_std, 1, 3}, {"var", f_var, 1, 3},
        {"min", f_min, 1, -1}, {"max", f_max, 1, -1}, {"amin", f_amin, 1, 3}, {"amax", f_amax, 1, 3},
        {"argmin", f_argmin, 1, 3}, {"argmax", f_argmax, 1, 3}, {"any", f_any, 1, 3}, {"all", f_all, 1, 3},
        {"ptp", f_ptp, 1, 3}, {"cumsum", f_cumsum, 1, 2}, {"cumprod", f_cumprod, 1, 2},
        {"count_nonzero", f_count_nonzero, 1, 3}, {"average", f_average, 1, 3}, {"median", f_median, 1, 2},
        {"percentile", f_percentile, 2, 3}, {"quantile", f_quantile, 2, 3},
        {"sort", f_sort, 1, 2}, {"argsort", f_argsort, 1, 2}, {"unique", f_unique, 1, 2},
        {"searchsorted", f_searchsorted, 2, 3}, {"bincount", f_bincount, 1, 2},
        {"matmul", f_matmul, 2, 2}, {"dot", f_dot, 2, 2}, {"inner", f_inner, 2, 2}, {"outer", f_outer, 2, 2},
        {"trace", f_trace, 1, 1}, {"det", f_det, 1, 1}, {"slogdet", f_slogdet, 1, 1}, {"inv", f_inv, 1, 1},
        {"solve", f_solve, 2, 2}, {"cholesky", f_cholesky, 1, 1}, {"qr", f_qr, 1, 2}, {"svd", f_svd, 1, 2},
        {"eigh", f_eigh, 1, 1}, {"eigvalsh", f_eigvalsh, 1, 1}, {"lstsq", f_lstsq, 2, 3}, {"pinv", f_pinv, 1, 2},
        {"matrix_rank", f_matrix_rank, 1, 1}, {"norm", f_norm, 1, 3},
        {"seed", f_seed, 0, 1}, {"rand", f_rand, 0, -1}, {"randn", f_randn, 0, -1}, {"random", f_random, 0, 1},
        {"random_sample", f_random, 0, 1}, {"standard_normal", f_standard_normal, 0, 1}, {"uniform", f_uniform, 0, 3},
        {"normal", f_normal, 0, 3}, {"exponential", f_exponential, 0, 2}, {"randint", f_randint, 1, 3},
        {"choice", f_choice, 1, 3}, {"permutation", f_permutation, 1, 1}, {"shuffle", f_permutation, 1, 1},
        {"save", f_save, 2, 2}, {"load", f_load, 1, 1}, {"loadtxt", f_loadtxt, 1, 2}, {"savetxt", f_savetxt, 2, 3},
        {"load_safetensors", f_load_safetensors, 1, 1}, {"save_safetensors", f_save_safetensors, 2, 2},
        {"blas", f_blas, 0, 0}, {"use_blas", f_use_blas, 1, 1}, {"threads", f_threads, 0, 1},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    api->constant(plugin, "pi", 3.141592653589793);
    api->constant(plugin, "e", 2.718281828459045);
    api->constant(plugin, "inf", INFINITY);
    api->constant(plugin, "nan", NAN);
    struct { const char *name; LubeFn fn; int min, max; } methods[] = {
        {"reshape", m_reshape, 2, -1}, {"flatten", m_flatten, 1, 1}, {"ravel", m_ravel, 1, 1},
        {"transpose", m_transpose, 1, -1}, {"swapaxes", m_swapaxes, 3, 3}, {"squeeze", m_squeeze, 1, 2},
        {"astype", m_astype, 2, 2}, {"copy", m_copy, 1, 1}, {"to_list", m_to_list, 1, 1}, {"tolist", m_to_list, 1, 1},
        {"to_bytes", m_to_bytes, 1, 1}, {"tobytes", m_to_bytes, 1, 1}, {"item", m_item, 1, 2},
        {"sum", m_sum, 1, 3}, {"prod", m_prod, 1, 3}, {"mean", m_mean, 1, 3}, {"std", m_std, 1, 3}, {"var", m_var, 1, 3},
        {"min", m_min, 1, 3}, {"max", m_max, 1, 3}, {"argmin", m_argmin, 1, 3}, {"argmax", m_argmax, 1, 3},
        {"any", m_any, 1, 3}, {"all", m_all, 1, 3}, {"cumsum", m_cumsum, 1, 2}, {"cumprod", m_cumprod, 1, 2},
        {"dot", m_dot, 2, 2}, {"clip", m_clip, 3, 3}, {"round", m_round, 1, 2}, {"sort", m_sort, 1, 2},
        {"argsort", m_argsort, 1, 2}, {"nonzero", m_nonzero, 1, 1},
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++)
        api->method(ArrayType, methods[i].name, methods[i].fn, methods[i].min, methods[i].max);
    api->property(ArrayType, "shape", p_shape);
    api->property(ArrayType, "ndim", p_ndim);
    api->property(ArrayType, "size", p_size);
    api->property(ArrayType, "dtype", p_dtype);
    api->property(ArrayType, "T", p_T);
    api->property(ArrayType, "itemsize", p_itemsize);
    api->property(ArrayType, "nbytes", p_nbytes);
    struct { const char *op; LubeFn fn; } ops[] = {
        {"+", o_add}, {"-", o_sub}, {"*", o_mul}, {"/", o_div}, {"%", o_mod}, {"**", o_pow}, {"@", o_matmul},
        {"==", o_eq}, {"!=", o_ne}, {"<", o_lt}, {"<=", o_le}, {">", o_gt}, {">=", o_ge}, {"neg", o_neg},
        {"[]", o_get}, {"[]=", o_set}, {"len", o_len}, {"copy", o_copy}, {"+=", i_add}, {"-=", i_sub},
        {"*=", i_mul}, {"/=", i_div}, {"%=", i_mod},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) api->op(ArrayType, ops[i].op, ops[i].fn);
    return LUBE_PLUGIN_API;
}
