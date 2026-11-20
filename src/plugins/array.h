/*
 * array.h - the plugin array's Arrays, for other plugins (nn, learn, llm).
 *
 * A plugin that works with Arrays gets this table in its lube_plugin_init:
 *
 *   static const ArrayExports *arr;
 *   arr = (const ArrayExports *)api->require(plugin, "array");
 *   if (!arr || arr->version < ARRAY_EXPORTS) return -1;
 *
 * An Array is n-dimensional, of one dtype, and may be a view: it shares the
 * memory of another (data points into it, and strides say where each next
 * element is). Copies are made when one that shares memory changes, so
 * Arrays behave as values, as Lube's lists do.
 */
#ifndef LUBE_ARRAY_H
#define LUBE_ARRAY_H

#include "../lube_plugin.h"

#define ARRAY_EXPORTS 1  /* this table's version */
#define ARR_MAXDIM 16    /* the most dimensions an Array has */

enum { ARR_F64, ARR_F32, ARR_I64, ARR_I32, ARR_U8, ARR_BOOL, ARR_NDTYPES };  /* dtypes */

/* elementwise operations of two operands (ArrayExports.binary) */
enum {
    ARR_ADD, ARR_SUB, ARR_MUL, ARR_DIV, ARR_MOD, ARR_POW, ARR_MAXIMUM, ARR_MINIMUM, ARR_ATAN2, ARR_HYPOT,
    ARR_FMOD, ARR_FLOOR_DIVIDE, ARR_COPYSIGN, ARR_LT, ARR_LE, ARR_GT, ARR_GE, ARR_EQ, ARR_NE,
    ARR_AND, ARR_OR, ARR_XOR, ARR_NBINARY
};

/* elementwise operations of one (ArrayExports.unary) */
enum {
    ARR_NEG, ARR_ABS, ARR_SQRT, ARR_EXP, ARR_LOG, ARR_LOG2, ARR_LOG10, ARR_LOG1P, ARR_EXPM1, ARR_SIN, ARR_COS,
    ARR_TAN, ARR_ASIN, ARR_ACOS, ARR_ATAN, ARR_SINH, ARR_COSH, ARR_TANH, ARR_ASINH, ARR_ACOSH, ARR_ATANH,
    ARR_FLOOR, ARR_CEIL, ARR_TRUNC, ARR_RINT, ARR_SIGN, ARR_SQUARE, ARR_RECIPROCAL, ARR_CBRT, ARR_EXP2, ARR_ERF,
    ARR_ISNAN, ARR_ISINF, ARR_ISFINITE, ARR_NOT, ARR_NUNARY
};

/* reductions (ArrayExports.reduce) */
enum { ARR_SUM, ARR_PROD, ARR_MEAN, ARR_MIN, ARR_MAX, ARR_ARGMIN, ARR_ARGMAX, ARR_ANY, ARR_ALL, ARR_VAR, ARR_STD,
       ARR_NREDUCE };
#define ARR_ALL_AXES (-1000)  /* reduce's axis for every element at once */

typedef struct ArrBuffer ArrBuffer;  /* memory, shared by the Arrays that view it */

typedef struct {
    ArrBuffer *buf;
    unsigned char *data;             /* the first element */
    int dtype, ndim;
    int64_t size;                    /* how many elements: the product of shape */
    int64_t shape[ARR_MAXDIM];
    int64_t strides[ARR_MAXDIM];     /* in elements, not bytes */
} Array;

typedef struct {
    int version;                     /* ARRAY_EXPORTS */
    const LubeType *type;            /* Array */
    const char *const *dtype_names;  /* "float64", "float32", "int64", "int32", "uint8", "bool" */
    const int *itemsize;             /* each dtype's bytes */
    /* v's Array, or NULL if it is none */
    Array *(*get)(LubeValue v);
    /* A new Array of zeros, C-contiguous; *data (if given) is its memory */
    LubeValue (*make)(LubeVM *vm, int dtype, int ndim, const int64_t *shape, void **data);
    /* array(x, dtype): anything array() takes (an Array, a Number, nested lists...); dtype -1 keeps x's */
    LubeValue (*from)(LubeVM *vm, LubeValue x, int dtype);
    /* x as a C-contiguous Array of dtype (-1: its own): x itself (retained) when it is one already */
    LubeValue (*contiguous)(LubeVM *vm, LubeValue x, int dtype);
    /* a op b, elementwise, broadcasting (ARR_ADD...); either may be a Number */
    LubeValue (*binary)(LubeVM *vm, int op, LubeValue a, LubeValue b);
    /* op(a), elementwise (ARR_NEG...) */
    LubeValue (*unary)(LubeVM *vm, int op, LubeValue a);
    /* a @ b, as matmul() */
    LubeValue (*matmul)(LubeVM *vm, LubeValue a, LubeValue b);
    /* a reduction (ARR_SUM...) over one axis, or ARR_ALL_AXES; gives an Array, 0-dimensional for all axes */
    LubeValue (*reduce)(LubeVM *vm, int op, LubeValue a, int axis, int keepdims);
    /* a with another shape (one entry may be -1); a view when it can be */
    LubeValue (*reshape)(LubeVM *vm, LubeValue a, int ndim, const int64_t *shape);
    /* a's axes in another order (axes NULL: reversed); a view */
    LubeValue (*transpose)(LubeVM *vm, LubeValue a, const int *axes);
    /* a summed down to shape, undoing broadcasting (for gradients) */
    LubeValue (*sum_to)(LubeVM *vm, LubeValue a, int ndim, const int64_t *shape);
    /* a[...] with argv as Lube passes it (argv[0] is a) */
    LubeValue (*index)(LubeVM *vm, int argc, const LubeValue *argv);
    /* runs fn(ctx, from, to) over 0..n in pieces on several threads (pieces of at least `grain`) */
    void (*parallel)(int64_t n, int64_t grain, void (*fn)(void *ctx, int64_t from, int64_t to), void *ctx);
    /* Makes a's memory its own (C-contiguous) before the plugin changes it in place: a copy if it is shared */
    void (*own)(Array *a);
    /* n random numbers of dtype at data, from array's generator (seed() sets it): uniform in [a, b), or normal
     * with mean a and deviation b */
    void (*fill_random)(void *data, int dtype, int64_t n, int normal, double a, double b);
    /* load_safetensors(path) and save_safetensors(path, obj), as the package's functions */
    LubeValue (*load_safetensors)(LubeVM *vm, const char *path);
    LubeValue (*save_safetensors)(LubeVM *vm, const char *path, LubeValue obj);
    /* seed(n): the generator fill_random draws from */
    void (*seed)(uint32_t n);
    /* How a prints, as print(a) shows it: snprintf's count */
    int (*describe)(const Array *a, char *buf, size_t size);
} ArrayExports;

#endif
