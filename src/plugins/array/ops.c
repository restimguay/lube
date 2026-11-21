/*
 * array/ops.c - elementwise operations (a + b, sqrt(a)...), broadcasting as
 * numpy does, and reductions (sum, mean, max, argmax...) over all elements
 * or along one axis. (Part of array.c.)
 */

static const char *const BINARY_NAMES[ARR_NBINARY] = {
    "+", "-", "*", "/", "%", "**", "maximum", "minimum", "arctan2", "hypot", "fmod", "floor_divide", "copysign",
    "<", "<=", ">", ">=", "==", "!=", "logical_and", "logical_or", "logical_xor"};

/* numpy's promotion of two dtypes: the smallest that holds both (int32 and float32 need float64). */
static int promote(int a, int b) {
    static const int rank[ARR_NDTYPES] = {5, 4, 3, 2, 1, 0};  /* f64 f32 i64 i32 u8 bool */
    if (a == b) return a;
    if (a == ARR_F64 || b == ARR_F64) return ARR_F64;
    if (a == ARR_F32 || b == ARR_F32) {
        int other = a == ARR_F32 ? b : a;
        return other == ARR_I64 || other == ARR_I32 ? ARR_F64 : ARR_F32;
    }
    return rank[a] > rank[b] ? a : b;
}

/* An operand of an elementwise operation: an Array, or a Lube Number or Bool, which takes the Array's dtype
 * where it fits (numpy's rule for Python's numbers: float32 + 0.5 stays float32). */
typedef struct {
    Array *a;           /* its Array (NULL for a Number or Bool) */
    LubeValue keep;     /* an Array made for it (a list's), to let go of */
    int made;
    double num;         /* the Number (a Bool: 0 or 1) */
    int is_bool;
} Operand;

static int operand_of(LubeVM *vm, LubeValue v, Operand *o, LubeValue *err) {
    memset(o, 0, sizeof *o);
    int k = lube->kind(v);
    if ((o->a = arr_of(v)) != NULL) return 1;
    if (k == LUBE_NUMBER || k == LUBE_BOOL) {
        o->num = k == LUBE_BOOL ? lube->truthy(v) : lube->number(v);
        o->is_bool = k == LUBE_BOOL;
        return 1;
    }
    double s, e, st;
    int incl;
    if (k == LUBE_LIST || lube->range(v, &s, &e, &st, &incl)) {
        o->keep = arr_from(vm, v, -1, &o->a);
        if (!o->a) {
            *err = o->keep;
            return 0;
        }
        o->made = 1;
        return 1;
    }
    *err = lube->error(vm, "expected an Array, a Number or a list, got %s", lube->a_type(v));
    return 0;
}

static void operand_done(Operand *o) {
    if (o->made) lube->release(o->keep);
}

/* The dtype an operand counts as, beside the other's. */
static int operand_dtype(const Operand *o, const Operand *other) {
    if (o->a) return o->a->dtype;
    if (o->is_bool) return other->a ? other->a->dtype : ARR_BOOL;
    if (!other->a) return ARR_F64;
    int t = other->a->dtype;
    if (IS_FLOAT(t)) return t;
    if (o->num == floor(o->num) && fabs(o->num) < 9.2e18) return t == ARR_BOOL ? ARR_I64 : t;
    return ARR_F64;
}

/* ---------- the kernels: one innermost run of an operation ---------- */

typedef struct {
    int op, kind;  /* the operation, and the dtype it computes in (f64, f32, i64, or bool for logic) */
} OpCtx;

/* o[i] = EXPR(x, y) over the run: operand 0 is the result, 1 and 2 the inputs; fast when they are in a row
 * or one of them is a single value (stride 0). */
#define BLOOP(T, OT, EXPR)                                                                                     \
    do {                                                                                                       \
        const T *A = (const T *)p[1], *B = (const T *)p[2];                                                    \
        OT *O = (OT *)p[0];                                                                                    \
        int64_t sa = st[1], sb = st[2], so = st[0];                                                            \
        if (sa == 1 && sb == 1 && so == 1) {                                                                   \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T x = A[i], y = B[i];                                                                          \
                O[i] = (OT)(EXPR);                                                                             \
            }                                                                                                  \
        } else if (sa == 1 && sb == 0 && so == 1) {                                                            \
            T y = B[0];                                                                                        \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T x = A[i];                                                                                    \
                O[i] = (OT)(EXPR);                                                                             \
            }                                                                                                  \
        } else if (sa == 0 && sb == 1 && so == 1) {                                                            \
            T x = A[0];                                                                                        \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T y = B[i];                                                                                    \
                O[i] = (OT)(EXPR);                                                                             \
            }                                                                                                  \
        } else {                                                                                               \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T x = A[i * sa], y = B[i * sb];                                                                \
                O[i * so] = (OT)(EXPR);                                                                        \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)

static double py_mod(double x, double y) {  /* the sign of y, as Python's and Lube's % */
    double r = fmod(x, y);
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return r == 0 ? copysign(0.0, y) : r;
}

static int64_t imod(int64_t x, int64_t y) {
    if (y == 0 || y == -1) return 0;  /* (by zero: 0, as numpy gives; -1: no overflow) */
    int64_t r = x % y;
    return r != 0 && ((r < 0) != (y < 0)) ? r + y : r;
}

static int64_t ifloordiv(int64_t x, int64_t y) {
    if (y == 0) return 0;
    if (y == -1) return (int64_t)(0 - (uint64_t)x);
    int64_t q = x / y;
    return x % y != 0 && ((x < 0) != (y < 0)) ? q - 1 : q;
}

static int64_t ipow(int64_t x, int64_t y) {  /* (y >= 0: checked before) */
    uint64_t r = 1, b = (uint64_t)x;
    while (y > 0) {
        if (y & 1) r *= b;
        b *= b;
        y >>= 1;
    }
    return (int64_t)r;
}

#define WRAP(op) ((int64_t)((uint64_t)x op (uint64_t)y))  /* (integers wrap around, as numpy's) */
#define FMAX(x, y) ((x) > (y) || (x) != (x) ? (x) : (y))   /* (NaN wins, as numpy's maximum) */
#define FMIN(x, y) ((x) < (y) || (x) != (x) ? (x) : (y))

static void bin_run(void *ctx, int64_t n, unsigned char *const *p, const int64_t *st) {
    const OpCtx *c = (const OpCtx *)ctx;
    if (c->kind == ARR_F64) {
        switch (c->op) {
        case ARR_ADD: BLOOP(double, double, x + y); break;
        case ARR_SUB: BLOOP(double, double, x - y); break;
        case ARR_MUL: BLOOP(double, double, x * y); break;
        case ARR_DIV: BLOOP(double, double, x / y); break;
        case ARR_MOD: BLOOP(double, double, py_mod(x, y)); break;
        case ARR_POW:
            if (st[2] == 0 && *(const double *)p[2] == 2) BLOOP(double, double, ((void)y, x * x));  /* (x ** 2: the square, as numpy) */
            else BLOOP(double, double, pow(x, y));
            break;
        case ARR_MAXIMUM: BLOOP(double, double, FMAX(x, y)); break;
        case ARR_MINIMUM: BLOOP(double, double, FMIN(x, y)); break;
        case ARR_ATAN2: BLOOP(double, double, atan2(x, y)); break;
        case ARR_HYPOT: BLOOP(double, double, hypot(x, y)); break;
        case ARR_FMOD: BLOOP(double, double, fmod(x, y)); break;
        case ARR_FLOOR_DIVIDE: BLOOP(double, double, floor(x / y)); break;
        case ARR_COPYSIGN: BLOOP(double, double, copysign(x, y)); break;
        case ARR_LT: BLOOP(double, unsigned char, x < y); break;
        case ARR_LE: BLOOP(double, unsigned char, x <= y); break;
        case ARR_GT: BLOOP(double, unsigned char, x > y); break;
        case ARR_GE: BLOOP(double, unsigned char, x >= y); break;
        case ARR_EQ: BLOOP(double, unsigned char, x == y); break;
        default: BLOOP(double, unsigned char, x != y); break;  /* ARR_NE */
        }
    } else if (c->kind == ARR_F32) {
        switch (c->op) {
        case ARR_ADD: BLOOP(float, float, x + y); break;
        case ARR_SUB: BLOOP(float, float, x - y); break;
        case ARR_MUL: BLOOP(float, float, x * y); break;
        case ARR_DIV: BLOOP(float, float, x / y); break;
        case ARR_MOD: BLOOP(float, float, py_mod(x, y)); break;
        case ARR_POW:
            if (st[2] == 0 && *(const float *)p[2] == 2) BLOOP(float, float, ((void)y, x * x));
            else BLOOP(float, float, powf(x, y));
            break;
        case ARR_MAXIMUM: BLOOP(float, float, FMAX(x, y)); break;
        case ARR_MINIMUM: BLOOP(float, float, FMIN(x, y)); break;
        case ARR_ATAN2: BLOOP(float, float, atan2f(x, y)); break;
        case ARR_HYPOT: BLOOP(float, float, hypotf(x, y)); break;
        case ARR_FMOD: BLOOP(float, float, fmodf(x, y)); break;
        case ARR_FLOOR_DIVIDE: BLOOP(float, float, floorf(x / y)); break;
        case ARR_COPYSIGN: BLOOP(float, float, copysignf(x, y)); break;
        case ARR_LT: BLOOP(float, unsigned char, x < y); break;
        case ARR_LE: BLOOP(float, unsigned char, x <= y); break;
        case ARR_GT: BLOOP(float, unsigned char, x > y); break;
        case ARR_GE: BLOOP(float, unsigned char, x >= y); break;
        case ARR_EQ: BLOOP(float, unsigned char, x == y); break;
        default: BLOOP(float, unsigned char, x != y); break;
        }
    } else if (c->kind == ARR_I64) {
        switch (c->op) {
        case ARR_ADD: BLOOP(int64_t, int64_t, WRAP(+)); break;
        case ARR_SUB: BLOOP(int64_t, int64_t, WRAP(-)); break;
        case ARR_MUL: BLOOP(int64_t, int64_t, WRAP(*)); break;
        case ARR_MOD: BLOOP(int64_t, int64_t, imod(x, y)); break;
        case ARR_POW: BLOOP(int64_t, int64_t, ipow(x, y)); break;
        case ARR_MAXIMUM: BLOOP(int64_t, int64_t, x > y ? x : y); break;
        case ARR_MINIMUM: BLOOP(int64_t, int64_t, x < y ? x : y); break;
        case ARR_FLOOR_DIVIDE: BLOOP(int64_t, int64_t, ifloordiv(x, y)); break;
        case ARR_LT: BLOOP(int64_t, unsigned char, x < y); break;
        case ARR_LE: BLOOP(int64_t, unsigned char, x <= y); break;
        case ARR_GT: BLOOP(int64_t, unsigned char, x > y); break;
        case ARR_GE: BLOOP(int64_t, unsigned char, x >= y); break;
        case ARR_EQ: BLOOP(int64_t, unsigned char, x == y); break;
        default: BLOOP(int64_t, unsigned char, x != y); break;
        }
    } else {  /* bool: logic */
        switch (c->op) {
        case ARR_AND: BLOOP(unsigned char, unsigned char, x && y); break;
        case ARR_OR: BLOOP(unsigned char, unsigned char, x || y); break;
        default: BLOOP(unsigned char, unsigned char, (x != 0) != (y != 0)); break;  /* ARR_XOR */
        }
    }
}

static int is_compare(int op) { return op >= ARR_LT && op <= ARR_NE; }
static int is_logic(int op) { return op >= ARR_AND; }
static int float_only(int op) { return op == ARR_DIV || op == ARR_ATAN2 || op == ARR_HYPOT || op == ARR_FMOD || op == ARR_COPYSIGN; }

/* The dtype op computes in for operands of dtypes ta and tb, and the dtype of its result. */
static void bin_types(int op, int ta, int tb, int *kind, int *result) {
    int t = promote(ta, tb);
    if (is_logic(op)) {
        *kind = *result = ARR_BOOL;
        return;
    }
    if (float_only(op) && !IS_FLOAT(t)) t = ARR_F64;
    if (!is_compare(op) && t == ARR_BOOL) t = ARR_I64;  /* (true + true is 2) */
    *kind = IS_FLOAT(t) ? t : ARR_I64;
    *result = is_compare(op) ? ARR_BOOL : t;
}

/* An operand as an Array of the dtype an operation computes in (a single value for a Number). */
static LubeValue operand_as(LubeVM *vm, const Operand *o, int kind, Array **out) {
    if (!o->a) return scalar_array(vm, o->num, kind, out);
    if (o->a->dtype == kind) {
        *out = o->a;
        return lube->retain(lube->nothing());
    }
    return arr_copy(vm, o->a, kind, out);
}

/* Runs op over x and y into dst (its shape is theirs broadcast; its dtype the kind's, or bool). */
static void bin_into(Array *dst, int op, int kind, const Array *x, const Array *y) {
    LoopSpec s;
    s.nd = dst->ndim;
    for (int d = 0; d < dst->ndim; d++) {
        s.shape[d] = dst->shape[d];
        s.st[0][d] = dst->strides[d];
    }
    broadcast_strides(x, dst->ndim, dst->shape, s.st[1]);
    broadcast_strides(y, dst->ndim, dst->shape, s.st[2]);
    unsigned char *base[3] = {dst->data, x->data, y->data};
    int size[3] = {ITEMSIZE[dst->dtype], ITEMSIZE[x->dtype], ITEMSIZE[y->dtype]};
    Loop L;
    loop_make(&L, &s, 3, base, size);
    OpCtx c = {op, kind};
    int heavy = op == ARR_POW || op == ARR_ATAN2 || op == ARR_HYPOT || op == ARR_FMOD || op == ARR_MOD ||
                op == ARR_FLOOR_DIVIDE;
    loop_go(&L, bin_run, &c, heavy && IS_FLOAT(kind) ? 32 : 1);
}

/* x op y, elementwise. inplace: into x (an Array of the result's dtype and shape: 1 if it was so). Two Numbers
 * give a Number. */
static LubeValue bin_op(LubeVM *vm, int op, LubeValue xv, LubeValue yv, int inplace, int *done) {
    Operand ox, oy;
    LubeValue err;
    if (done) *done = 0;
    if (!operand_of(vm, xv, &ox, &err)) return err;
    if (!operand_of(vm, yv, &oy, &err)) {
        operand_done(&ox);
        return err;
    }
    int kind, result;
    bin_types(op, operand_dtype(&ox, &oy), operand_dtype(&oy, &ox), &kind, &result);
    Array *x, *y;
    LubeValue kx = operand_as(vm, &ox, kind, &x), ky = operand_as(vm, &oy, kind, &y);
    LubeValue r;
    int nd;
    int64_t shape[ARR_MAXDIM];
    if (!broadcast(x, y, &nd, shape)) {
        r = broadcast_error(vm, ox.a ? ox.a : x, oy.a ? oy.a : y);
        goto out;
    }
    if (op == ARR_POW && kind == ARR_I64) {  /* integers to negative powers: no integer answer */
        for (int64_t i = 0; i < y->size; i++)
            if (load_int(elem_at(y, i), ARR_I64) < 0) {
                r = lube->error(vm, "integers cannot be raised to negative powers (use a float dtype)");
                goto out;
            }
    }
    Array *dst = ox.a;
    if (inplace && dst->dtype == (is_compare(op) ? ARR_BOOL : kind) && dst->dtype == result && nd == dst->ndim &&
        !memcmp(shape, dst->shape, sizeof(int64_t) * (size_t)nd)) {
        arr_own(dst);
        if (x == ox.a) x = dst;  /* (it may have moved) */
        bin_into(dst, op, kind, x, y);
        *done = 1;
        r = lube->retain(xv);
        goto out;
    }
    int write = is_compare(op) || is_logic(op) ? ARR_BOOL : kind;
    r = arr_new(vm, write, nd, shape, &dst, 0);
    if (!dst) goto out;
    bin_into(dst, op, kind, x, y);
    if (write != result) {  /* int32 or uint8: computed in int64 */
        Array *conv;
        LubeValue c = arr_copy(vm, dst, result, &conv);
        lube->release(r);
        r = c;
    }
    if (!ox.a && !oy.a) {  /* two Numbers: a Number */
        LubeValue num = elem_value(arr_of(r)->data, arr_of(r)->dtype);
        lube->release(r);
        r = num;
    }
out:
    lube->release(kx);
    lube->release(ky);
    operand_done(&ox);
    operand_done(&oy);
    return r;
}

/* ---------- one operand ---------- */

static const char *const UNARY_NAMES[ARR_NUNARY] = {
    "negative", "abs", "sqrt", "exp", "log", "log2", "log10", "log1p", "expm1", "sin", "cos", "tan", "arcsin",
    "arccos", "arctan", "sinh", "cosh", "tanh", "arcsinh", "arccosh", "arctanh", "floor", "ceil", "trunc", "rint",
    "sign", "square", "reciprocal", "cbrt", "exp2", "erf", "isnan", "isinf", "isfinite", "logical_not"};

#define ULOOP(T, OT, EXPR)                                                                                     \
    do {                                                                                                       \
        const T *A = (const T *)p[1];                                                                          \
        OT *O = (OT *)p[0];                                                                                    \
        int64_t sa = st[1], so = st[0];                                                                        \
        if (sa == 1 && so == 1) {                                                                              \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T x = A[i];                                                                                    \
                O[i] = (OT)(EXPR);                                                                             \
            }                                                                                                  \
        } else {                                                                                               \
            for (int64_t i = 0; i < n; i++) {                                                                  \
                T x = A[i * sa];                                                                               \
                O[i * so] = (OT)(EXPR);                                                                        \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)

#define SIGN(x) ((x) > 0 ? 1 : (x) < 0 ? -1 : (x))  /* (0 and NaN stay themselves) */

static void un_run(void *ctx, int64_t n, unsigned char *const *p, const int64_t *st) {
    const OpCtx *c = (const OpCtx *)ctx;
    if (c->kind == ARR_F64) {
        switch (c->op) {
        case ARR_NEG: ULOOP(double, double, -x); break;
        case ARR_ABS: ULOOP(double, double, fabs(x)); break;
        case ARR_SQRT: ULOOP(double, double, sqrt(x)); break;
        case ARR_EXP: ULOOP(double, double, exp(x)); break;
        case ARR_LOG: ULOOP(double, double, log(x)); break;
        case ARR_LOG2: ULOOP(double, double, log2(x)); break;
        case ARR_LOG10: ULOOP(double, double, log10(x)); break;
        case ARR_LOG1P: ULOOP(double, double, log1p(x)); break;
        case ARR_EXPM1: ULOOP(double, double, expm1(x)); break;
        case ARR_SIN: ULOOP(double, double, sin(x)); break;
        case ARR_COS: ULOOP(double, double, cos(x)); break;
        case ARR_TAN: ULOOP(double, double, tan(x)); break;
        case ARR_ASIN: ULOOP(double, double, asin(x)); break;
        case ARR_ACOS: ULOOP(double, double, acos(x)); break;
        case ARR_ATAN: ULOOP(double, double, atan(x)); break;
        case ARR_SINH: ULOOP(double, double, sinh(x)); break;
        case ARR_COSH: ULOOP(double, double, cosh(x)); break;
        case ARR_TANH: ULOOP(double, double, tanh(x)); break;
        case ARR_ASINH: ULOOP(double, double, asinh(x)); break;
        case ARR_ACOSH: ULOOP(double, double, acosh(x)); break;
        case ARR_ATANH: ULOOP(double, double, atanh(x)); break;
        case ARR_FLOOR: ULOOP(double, double, floor(x)); break;
        case ARR_CEIL: ULOOP(double, double, ceil(x)); break;
        case ARR_TRUNC: ULOOP(double, double, trunc(x)); break;
        case ARR_RINT: ULOOP(double, double, rint(x)); break;  /* (half to even) */
        case ARR_SIGN: ULOOP(double, double, SIGN(x)); break;
        case ARR_SQUARE: ULOOP(double, double, x * x); break;
        case ARR_RECIPROCAL: ULOOP(double, double, 1 / x); break;
        case ARR_CBRT: ULOOP(double, double, cbrt(x)); break;
        case ARR_EXP2: ULOOP(double, double, exp2(x)); break;
        case ARR_ERF: ULOOP(double, double, erf(x)); break;
        case ARR_ISNAN: ULOOP(double, unsigned char, x != x); break;
        case ARR_ISINF: ULOOP(double, unsigned char, isinf(x)); break;
        default: ULOOP(double, unsigned char, isfinite(x)); break;  /* ARR_ISFINITE */
        }
    } else if (c->kind == ARR_F32) {
        switch (c->op) {
        case ARR_NEG: ULOOP(float, float, -x); break;
        case ARR_ABS: ULOOP(float, float, fabsf(x)); break;
        case ARR_SQRT: ULOOP(float, float, sqrtf(x)); break;
        case ARR_EXP: ULOOP(float, float, expf(x)); break;
        case ARR_LOG: ULOOP(float, float, logf(x)); break;
        case ARR_LOG2: ULOOP(float, float, log2f(x)); break;
        case ARR_LOG10: ULOOP(float, float, log10f(x)); break;
        case ARR_LOG1P: ULOOP(float, float, log1pf(x)); break;
        case ARR_EXPM1: ULOOP(float, float, expm1f(x)); break;
        case ARR_SIN: ULOOP(float, float, sinf(x)); break;
        case ARR_COS: ULOOP(float, float, cosf(x)); break;
        case ARR_TAN: ULOOP(float, float, tanf(x)); break;
        case ARR_ASIN: ULOOP(float, float, asinf(x)); break;
        case ARR_ACOS: ULOOP(float, float, acosf(x)); break;
        case ARR_ATAN: ULOOP(float, float, atanf(x)); break;
        case ARR_SINH: ULOOP(float, float, sinhf(x)); break;
        case ARR_COSH: ULOOP(float, float, coshf(x)); break;
        case ARR_TANH: ULOOP(float, float, vm_tanh(x)); break;  /* (vecmath.h's: vectorized, the same bits everywhere) */
        case ARR_ASINH: ULOOP(float, float, asinhf(x)); break;
        case ARR_ACOSH: ULOOP(float, float, acoshf(x)); break;
        case ARR_ATANH: ULOOP(float, float, atanhf(x)); break;
        case ARR_FLOOR: ULOOP(float, float, floorf(x)); break;
        case ARR_CEIL: ULOOP(float, float, ceilf(x)); break;
        case ARR_TRUNC: ULOOP(float, float, truncf(x)); break;
        case ARR_RINT: ULOOP(float, float, rintf(x)); break;
        case ARR_SIGN: ULOOP(float, float, SIGN(x)); break;
        case ARR_SQUARE: ULOOP(float, float, x * x); break;
        case ARR_RECIPROCAL: ULOOP(float, float, 1 / x); break;
        case ARR_CBRT: ULOOP(float, float, cbrtf(x)); break;
        case ARR_EXP2: ULOOP(float, float, exp2f(x)); break;
        case ARR_ERF: ULOOP(float, float, erff(x)); break;
        case ARR_ISNAN: ULOOP(float, unsigned char, x != x); break;
        case ARR_ISINF: ULOOP(float, unsigned char, isinf(x)); break;
        default: ULOOP(float, unsigned char, isfinite(x)); break;
        }
    } else if (c->kind == ARR_I64) {
        switch (c->op) {
        case ARR_NEG: ULOOP(int64_t, int64_t, (int64_t)(0 - (uint64_t)x)); break;
        case ARR_ABS: ULOOP(int64_t, int64_t, x < 0 ? (int64_t)(0 - (uint64_t)x) : x); break;
        case ARR_SIGN: ULOOP(int64_t, int64_t, SIGN(x)); break;
        default: ULOOP(int64_t, int64_t, (int64_t)((uint64_t)x * (uint64_t)x)); break;  /* ARR_SQUARE */
        }
    } else {
        ULOOP(unsigned char, unsigned char, !x);  /* ARR_NOT */
    }
}

static LubeValue un_op(LubeVM *vm, int op, LubeValue xv) {
    Operand o, none;
    LubeValue err;
    if (!operand_of(vm, xv, &o, &err)) return err;
    memset(&none, 0, sizeof none);
    int t = operand_dtype(&o, &none), kind, result;
    if (op == ARR_NOT) {
        kind = result = ARR_BOOL;
    } else if (op == ARR_NEG || op == ARR_ABS || op == ARR_SIGN || op == ARR_SQUARE) {
        kind = IS_FLOAT(t) ? t : ARR_I64;
        result = t == ARR_BOOL ? ARR_I64 : t;
    } else if (op >= ARR_FLOOR && op <= ARR_RINT && !IS_FLOAT(t)) {  /* whole already */
        kind = -1;
        result = t;
    } else {
        kind = IS_FLOAT(t) ? t : ARR_F64;
        result = op >= ARR_ISNAN ? ARR_BOOL : kind;
    }
    Array *x, *dst;
    LubeValue kx, r;
    if (kind < 0) {
        kx = operand_as(vm, &o, t, &x);
        r = arr_copy(vm, x, -1, &dst);
    } else {
        kx = operand_as(vm, &o, kind == ARR_BOOL ? ARR_BOOL : kind, &x);
        int write = op >= ARR_ISNAN ? ARR_BOOL : kind;
        r = arr_new(vm, write, x->ndim, x->shape, &dst, 0);
        if (dst) {
            LoopSpec s;
            s.nd = x->ndim;
            for (int d = 0; d < x->ndim; d++) {
                s.shape[d] = x->shape[d];
                s.st[0][d] = dst->strides[d];
                s.st[1][d] = x->strides[d];
            }
            unsigned char *base[2] = {dst->data, x->data};
            int size[2] = {ITEMSIZE[dst->dtype], ITEMSIZE[x->dtype]};
            Loop L;
            loop_make(&L, &s, 2, base, size);
            OpCtx c = {op, kind};
            int light = op == ARR_NEG || op == ARR_ABS || op == ARR_SQUARE || op == ARR_SIGN || op == ARR_NOT ||
                        op >= ARR_ISNAN || (op >= ARR_FLOOR && op <= ARR_RINT) || op == ARR_RECIPROCAL;
            loop_go(&L, un_run, &c, light ? 1 : op == ARR_SQRT ? 4 : 32);
            if (write != result) {
                Array *conv;
                LubeValue cv = arr_copy(vm, dst, result, &conv);
                lube->release(r);
                r = cv;
            }
        }
    }
    if (!o.a && arr_of(r)) {  /* a Number: a Number */
        LubeValue num = elem_value(arr_of(r)->data, arr_of(r)->dtype);
        lube->release(r);
        r = num;
    }
    lube->release(kx);
    operand_done(&o);
    return r;
}

/* ======================================================================
 * Reductions
 * ==================================================================== */

static const char *const REDUCE_NAMES[ARR_NREDUCE] = {"sum", "prod", "mean", "min", "max", "argmin", "argmax",
                                                      "any", "all", "var", "std"};

#define SUM_PIECE 4096  /* sums are added up in pieces of this many: the same pieces on any number of threads */

/* The sum of n doubles in a row: eight sums side by side (fast, and closer than one running sum). */
static double sum_f64(const double *x, int64_t n) {
    double s[8] = {0};
    int64_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) s[k] += x[i + k];
    double t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; i++) t += x[i];
    return t;
}

static double sum_f32(const float *x, int64_t n) {
    double s[8] = {0};
    int64_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) s[k] += x[i + k];
    double t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; i++) t += x[i];
    return t;
}

/* The sum of (x - c)^2 over n floats in a row, likewise in eight sums (var and std). */
#define SQUARES(T, NAME)                                                                                       \
    static double NAME(const T *x, int64_t n, double c) {                                                      \
        double s[8] = {0};                                                                                     \
        int64_t i = 0;                                                                                         \
        for (; i + 8 <= n; i += 8)                                                                             \
            for (int k = 0; k < 8; k++) {                                                                      \
                double d = x[i + k] - c;                                                                       \
                s[k] += d * d;                                                                                 \
            }                                                                                                  \
        double t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));                          \
        for (; i < n; i++) t += (x[i] - c) * (x[i] - c);                                                       \
        return t;                                                                                              \
    }
SQUARES(double, squares_f64)
SQUARES(float, squares_f32)

typedef struct {
    const unsigned char *data;
    int dtype;
    int64_t n;
    double *parts;
    int squares;      /* the squares' sum about center, not the sum */
    double center;
} SumJob;

/* The sum of x^2 (a center of 0: norm's), in eight sums. */
#define SQUARES0(T, NAME)                                                                                      \
    static double NAME(const T *x, int64_t n) {                                                                \
        double s[8] = {0};                                                                                     \
        int64_t i = 0;                                                                                         \
        for (; i + 8 <= n; i += 8)                                                                             \
            for (int k = 0; k < 8; k++) s[k] += (double)x[i + k] * x[i + k];                                   \
        double t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));                          \
        for (; i < n; i++) t += (double)x[i] * x[i];                                                           \
        return t;                                                                                              \
    }
SQUARES0(double, squares0_f64)
SQUARES0(float, squares0_f32)

static double sum_piece(const SumJob *j, int64_t at, int64_t len) {
    if (j->squares && j->center == 0)
        return j->dtype == ARR_F64 ? squares0_f64((const double *)j->data + at, len)
                                   : squares0_f32((const float *)j->data + at, len);
    if (j->squares)
        return j->dtype == ARR_F64 ? squares_f64((const double *)j->data + at, len, j->center)
                                   : squares_f32((const float *)j->data + at, len, j->center);
    return j->dtype == ARR_F64 ? sum_f64((const double *)j->data + at, len) : sum_f32((const float *)j->data + at, len);
}

static void sum_pieces(void *ctx, int64_t from, int64_t to) {
    SumJob *j = (SumJob *)ctx;
    for (int64_t p = from; p < to; p++) {
        int64_t at = p * SUM_PIECE, len = j->n - at < SUM_PIECE ? j->n - at : SUM_PIECE;
        j->parts[p] = sum_piece(j, at, len);
    }
}

/* The sum of a run of n floats (f64 or f32), or of their squares about center, in fixed pieces, those on
 * several threads when there are many. */
static double sum_or_squares(const unsigned char *data, int dtype, int64_t n, int squares, double center) {
    SumJob j = {data, dtype, n, NULL, squares, center};
    if (n <= SUM_PIECE) return sum_piece(&j, 0, n);
    int64_t pieces = (n + SUM_PIECE - 1) / SUM_PIECE;
    double small[64], *parts = pieces <= 64 ? small : (double *)malloc(sizeof(double) * (size_t)pieces);
    if (!parts) abort();  /* LCOV_EXCL_LINE: out of memory */
    j.parts = parts;
    par_for(pieces, PARALLEL_WORK / SUM_PIECE, sum_pieces, &j);
    double t = 0;
    for (int64_t p = 0; p < pieces; p++) t += parts[p];
    if (parts != small) free(parts);
    return t;
}

static double sum_run(const unsigned char *data, int dtype, int64_t n) { return sum_or_squares(data, dtype, n, 0, 0); }

/* The dtype a reduction gives for one of dtype t. */
static int reduce_dtype(int op, int t) {
    switch (op) {
    case ARR_SUM: case ARR_PROD: return IS_FLOAT(t) ? t : ARR_I64;
    case ARR_MEAN: case ARR_VAR: case ARR_STD: return t == ARR_F32 ? ARR_F32 : ARR_F64;
    case ARR_MIN: case ARR_MAX: return t;
    case ARR_ARGMIN: case ARR_ARGMAX: return ARR_I64;
    default: return ARR_BOOL;  /* any, all */
    }
}

#if defined(__aarch64__) && defined(__ARM_NEON)
/* The largest (or smallest) of n > 0 floats in a row, NaN if one is NaN: NEON's maximum and minimum keep a NaN,
 * so one pass finds both (and of 0 and -0 they take 0 as the larger). */
#define NEON_EXTREME(NAME, T, V, LD, DUP, MAX, MIN, LANES)                                                     \
    static T NAME(const T *x, int64_t n, int want_max) {                                                       \
        V a0 = DUP(x[0]), a1 = a0, a2 = a0, a3 = a0;                                                           \
        int64_t i = 0;                                                                                         \
        if (want_max)                                                                                          \
            for (; i + 4 * LANES <= n; i += 4 * LANES) {                                                       \
                a0 = MAX(a0, LD(x + i));                                                                       \
                a1 = MAX(a1, LD(x + i + LANES));                                                               \
                a2 = MAX(a2, LD(x + i + 2 * LANES));                                                           \
                a3 = MAX(a3, LD(x + i + 3 * LANES));                                                           \
            }                                                                                                  \
        else                                                                                                   \
            for (; i + 4 * LANES <= n; i += 4 * LANES) {                                                       \
                a0 = MIN(a0, LD(x + i));                                                                       \
                a1 = MIN(a1, LD(x + i + LANES));                                                               \
                a2 = MIN(a2, LD(x + i + 2 * LANES));                                                           \
                a3 = MIN(a3, LD(x + i + 3 * LANES));                                                           \
            }                                                                                                  \
        V a = want_max ? MAX(MAX(a0, a1), MAX(a2, a3)) : MIN(MIN(a0, a1), MIN(a2, a3));                        \
        T lanes[LANES];                                                                                        \
        memcpy(lanes, &a, sizeof lanes);                                                                       \
        T e = lanes[0];                                                                                        \
        for (int k = 1; k < LANES; k++) e = e != e || lanes[k] != lanes[k] ? NAN : want_max ? (lanes[k] > e ? lanes[k] : e) : (lanes[k] < e ? lanes[k] : e); \
        for (; i < n; i++) e = e != e || x[i] != x[i] ? NAN : want_max ? (x[i] > e ? x[i] : e) : (x[i] < e ? x[i] : e); \
        return e;                                                                                              \
    }
NEON_EXTREME(neon_extreme_f64, double, float64x2_t, vld1q_f64, vdupq_n_f64, vmaxq_f64, vminq_f64, 2)
NEON_EXTREME(neon_extreme_f32, float, float32x4_t, vld1q_f32, vdupq_n_f32, vmaxq_f32, vminq_f32, 4)
#endif

/* The code for each integer dtype (and bool), T its type, for a run of them at p. */
#define BY_INT(dt, ...)                                                                                        \
    do {                                                                                                       \
        if ((dt) == ARR_I64) {                                                                                 \
            typedef int64_t T;                                                                                 \
            __VA_ARGS__                                                                                        \
        } else if ((dt) == ARR_I32) {                                                                          \
            typedef int32_t T;                                                                                 \
            __VA_ARGS__                                                                                        \
        } else {                                                                                               \
            typedef unsigned char T;                                                                           \
            __VA_ARGS__                                                                                        \
        }                                                                                                      \
    } while (0)

/* The same for each dtype. */
#define BY_ANY(dt, ...)                                                                                        \
    do {                                                                                                       \
        if ((dt) == ARR_F64) {                                                                                 \
            typedef double T;                                                                                  \
            __VA_ARGS__                                                                                        \
        } else if ((dt) == ARR_F32) {                                                                          \
            typedef float T;                                                                                   \
            __VA_ARGS__                                                                                        \
        } else {                                                                                               \
            BY_INT(dt, __VA_ARGS__);                                                                           \
        }                                                                                                      \
    } while (0)

/* Reduces n elements in a row at p into one result written at out. (step: always 1, or n is 0: axes before the
 * last go by reduce_cols.) Loops for each dtype, so that they vectorize. */
static void reduce_one(int op, const unsigned char *p, int dtype, int64_t n, int64_t step, int ddof,
                       unsigned char *out, int rtype) {
    (void)step;
    int z = ITEMSIZE[dtype];
    switch (op) {
    case ARR_SUM: case ARR_MEAN: {
        if (!IS_FLOAT(dtype) && op == ARR_SUM) {  /* (whole numbers: exactly, wrapping as int64 does) */
            uint64_t s = 0;
            BY_INT(dtype, {
                const T *x = (const T *)p;
                for (int64_t i = 0; i < n; i++) s += (uint64_t)(int64_t)x[i];
            });
            store_int(out, rtype, (int64_t)s);
            break;
        }
        double s = 0;
        if (IS_FLOAT(dtype)) {
            s = sum_run(p, dtype, n);
        } else {
            BY_INT(dtype, {
                const T *x = (const T *)p;
                for (int64_t i = 0; i < n; i++) s += (double)x[i];
            });
        }
        store(out, rtype, op == ARR_MEAN ? s / (double)n : s);
        break;
    }
    case ARR_PROD: {
        if (!IS_FLOAT(dtype)) {
            uint64_t s = 1;
            BY_INT(dtype, {
                const T *x = (const T *)p;
                for (int64_t i = 0; i < n; i++) s *= (uint64_t)(int64_t)x[i];
            });
            store_int(out, rtype, (int64_t)s);
            break;
        }
        double s = 1;
        if (dtype == ARR_F64) for (int64_t i = 0; i < n; i++) s *= ((const double *)p)[i];
        else for (int64_t i = 0; i < n; i++) s *= ((const float *)p)[i];
        store(out, rtype, s);
        break;
    }
    case ARR_VAR: case ARR_STD: {
        double mean = 0, ss = 0;
        if (IS_FLOAT(dtype)) {  /* (as sum's: in fixed pieces, eight sums in each) */
            mean = sum_run(p, dtype, n) / (double)n;
            ss = sum_or_squares(p, dtype, n, 1, mean);
        } else {
            BY_INT(dtype, {
                const T *x = (const T *)p;
                for (int64_t i = 0; i < n; i++) mean += (double)x[i];
                mean /= (double)n;
                for (int64_t i = 0; i < n; i++) ss += ((double)x[i] - mean) * ((double)x[i] - mean);
            });
        }
        double v = n - ddof > 0 ? ss / (double)(n - ddof) : NAN;
        store(out, rtype, op == ARR_STD ? sqrt(v) : v);
        break;
    }
    case ARR_MIN: case ARR_MAX: case ARR_ARGMIN: case ARR_ARGMAX: {
        /* In two passes that vectorize (the extreme, or a NaN; then the first place it is): what one pass taking
         * each strictly better one would pick. */
        int want_max = op == ARR_MAX || op == ARR_ARGMAX, nan = 0, arg = op == ARR_ARGMIN || op == ARR_ARGMAX, exact = 0;
        int64_t best = 0;
        /* (eight extremes side by side, by fmax and fmin for floats, which vectorize: NaN is looked for apart,
         * and of 0 and -0 the first in the row is taken anyway) */
#define EXTREME(T, BIGGER, SMALLER)                                                                            \
    do {                                                                                                       \
        const T *x = (const T *)p;                                                                             \
        T e = x[0], ek[8];                                                                                     \
        int nk[8] = {0};                                                                                       \
        int64_t i = 0;                                                                                         \
        for (int k = 0; k < 8; k++) ek[k] = x[0];                                                              \
        if (want_max) {                                                                                        \
            for (; i + 8 <= n; i += 8)                                                                         \
                for (int k = 0; k < 8; k++) {                                                                  \
                    ek[k] = BIGGER(ek[k], x[i + k]);                                                           \
                    nk[k] |= x[i + k] != x[i + k];                                                             \
                }                                                                                              \
            for (int k = 0; k < 8; k++) e = BIGGER(e, ek[k]);                                                  \
            for (; i < n; i++) e = BIGGER(e, x[i]);                                                            \
        } else {                                                                                               \
            for (; i + 8 <= n; i += 8)                                                                         \
                for (int k = 0; k < 8; k++) {                                                                  \
                    ek[k] = SMALLER(ek[k], x[i + k]);                                                          \
                    nk[k] |= x[i + k] != x[i + k];                                                             \
                }                                                                                              \
            for (int k = 0; k < 8; k++) e = SMALLER(e, ek[k]);                                                 \
            for (; i < n; i++) e = SMALLER(e, x[i]);                                                           \
        }                                                                                                      \
        for (int k = 0; k < 8; k++) nan |= nk[k];                                                              \
        for (i = n & ~(int64_t)7; i < n; i++) nan |= x[i] != x[i];                                             \
        nan |= x[0] != x[0];                                                                                   \
        if (nan) {                                                                                             \
            while (x[best] == x[best]) best++;                                                                 \
        } else if (arg || e == 0) {  /* (where it is: for argmin and argmax, and for 0, whose sign is the first's) */ \
            while (x[best] != e) best++;                                                                       \
        } else {                                                                                               \
            exact = 1;                                                                                         \
            if (IS_FLOAT(dtype)) store(out, rtype, (double)e);  /* (the very value: exact in its dtype) */          \
            else store_int(out, rtype, (int64_t)e);                                                            \
        }                                                                                                      \
    } while (0)
#define IBIG(a, b) ((b) > (a) ? (b) : (a))
#define ISMALL(a, b) ((b) < (a) ? (b) : (a))
#if defined(__aarch64__) && defined(__ARM_NEON)
#define FLOAT_EXTREME(T, FN)                                                                                   \
    do {                                                                                                       \
        const T *x = (const T *)p;                                                                             \
        T e = FN(x, n, want_max);                                                                              \
        if (e != e) {                                                                                          \
            nan = 1;                                                                                           \
            while (x[best] == x[best]) best++;                                                                 \
        } else if (arg || e == 0) {                                                                            \
            while (x[best] != e) best++;                                                                       \
        } else {                                                                                               \
            exact = 1;                                                                                         \
            store(out, rtype, (double)e);                                                                      \
        }                                                                                                      \
    } while (0)
        if (dtype == ARR_F64) FLOAT_EXTREME(double, neon_extreme_f64);
        else if (dtype == ARR_F32) FLOAT_EXTREME(float, neon_extreme_f32);
#undef FLOAT_EXTREME
#else
        if (dtype == ARR_F64) EXTREME(double, fmax, fmin);
        else if (dtype == ARR_F32) EXTREME(float, fmaxf, fminf);
#endif
        else BY_INT(dtype, { EXTREME(T, IBIG, ISMALL); });
#undef EXTREME
#undef IBIG
#undef ISMALL
        if (arg) store_int(out, rtype, best);
        else if (!exact) memcpy(out, p + best * z, (size_t)z);
        break;
    }
    default: {  /* any, all: in blocks, stopping at the first that settles it */
        int any = op == ARR_ANY, r = !any;
        if (z == 1) {  /* (bytes: all is no 0 among them, any a word of them that is not 0) */
            if (!any) {
                *out = memchr(p, 0, (size_t)n) == NULL;
                break;
            }
            int64_t i = 0;
            uint64_t w = 0;
            for (; i + 8 <= n && !w; i += 8) {
                uint64_t v;
                memcpy(&v, p + i, 8);
                w |= v;
            }
            for (; i < n && !w; i++) w |= p[i];
            *out = w != 0;
            break;
        }
        for (int64_t at = 0; at < n && r != any; at += 4096) {
            int64_t m = n - at < 4096 ? n - at : 4096, hits = 0;
            BY_ANY(dtype, {
                const T *x = (const T *)p + at;
                for (int64_t i = 0; i < m; i++) hits += x[i] != 0;
            });
            if (any ? hits > 0 : hits < m) r = any;
        }
        *out = (unsigned char)r;
        break;
    }
    }
}

typedef struct {
    int op, dtype, rtype, ddof;
    const unsigned char *data;
    unsigned char *out;
    int64_t n, inner;
} ReduceJob;

static void reduce_rows(void *ctx, int64_t from, int64_t to) {
    ReduceJob *j = (ReduceJob *)ctx;
    int z = ITEMSIZE[j->dtype], rz = ITEMSIZE[j->rtype];
    for (int64_t r = from; r < to; r++) {  /* r: an outer position and an inner one */
        int64_t o = r / j->inner, i = r % j->inner;
        reduce_one(j->op, j->data + (o * j->n * j->inner + i) * z, j->dtype, j->n, j->inner, j->ddof,
                   j->out + r * rz, j->rtype);
    }
}

/* Along an axis that is not the last (inner > 1): row after row of a block of columns, each row's part loaded
 * into a buffer, the columns' accumulators beside it in the cache, so that every loop runs along memory. Each
 * column's arithmetic is reduce_one's, in the same order: the same results. Tasks: an outer position and a block
 * of RC_COLS inner ones. */
#define RC_COLS 256

static void load_row(double *x, const unsigned char *p, int dtype, int64_t m) {
    switch (dtype) {
    case ARR_F64: memcpy(x, p, sizeof(double) * (size_t)m); break;
    case ARR_F32: for (int64_t i = 0; i < m; i++) x[i] = ((const float *)p)[i]; break;
    case ARR_I64: for (int64_t i = 0; i < m; i++) x[i] = (double)((const int64_t *)p)[i]; break;
    case ARR_I32: for (int64_t i = 0; i < m; i++) x[i] = ((const int32_t *)p)[i]; break;
    default: for (int64_t i = 0; i < m; i++) x[i] = p[i]; break;  /* uint8, bool */
    }
}

static void load_row_int(int64_t *x, const unsigned char *p, int dtype, int64_t m) {
    switch (dtype) {
    case ARR_I64: memcpy(x, p, sizeof(int64_t) * (size_t)m); break;
    case ARR_I32: for (int64_t i = 0; i < m; i++) x[i] = ((const int32_t *)p)[i]; break;
    default: for (int64_t i = 0; i < m; i++) x[i] = p[i]; break;  /* uint8, bool */
    }
}

static void reduce_cols(void *ctx, int64_t from, int64_t to) {
    ReduceJob *j = (ReduceJob *)ctx;
    int op = j->op, dt = j->dtype, z = ITEMSIZE[dt], rz = ITEMSIZE[j->rtype], flt = IS_FLOAT(dt);
    int64_t n = j->n, inner = j->inner, blocks = (inner + RC_COLS - 1) / RC_COLS, rowstep = inner * z;
    double x[RC_COLS], acc[RC_COLS], acc2[RC_COLS];
    int64_t xi[RC_COLS], best[RC_COLS];
    uint64_t ua[RC_COLS];
    unsigned char flag[RC_COLS];
    for (int64_t t = from; t < to; t++) {
        int64_t o = t / blocks, i0 = (t % blocks) * RC_COLS, m = inner - i0 < RC_COLS ? inner - i0 : RC_COLS;
        const unsigned char *base = j->data + (o * n * inner + i0) * z;
        unsigned char *out = j->out + (o * inner + i0) * rz;
        switch (op) {
        case ARR_SUM: case ARR_PROD: case ARR_MEAN:
            if (!flt && op != ARR_MEAN) {  /* (whole numbers: exactly, wrapping as int64 does) */
                for (int64_t i = 0; i < m; i++) ua[i] = op == ARR_PROD;
                for (int64_t k = 0; k < n; k++) {
                    load_row_int(xi, base + k * rowstep, dt, m);
                    if (op == ARR_SUM) for (int64_t i = 0; i < m; i++) ua[i] += (uint64_t)xi[i];
                    else for (int64_t i = 0; i < m; i++) ua[i] *= (uint64_t)xi[i];
                }
                for (int64_t i = 0; i < m; i++) store_int(out + i * rz, j->rtype, (int64_t)ua[i]);
                break;
            }
            for (int64_t i = 0; i < m; i++) acc[i] = op == ARR_PROD;
            for (int64_t k = 0; k < n; k++) {
                if (dt == ARR_F64 && op != ARR_PROD) {  /* (straight from the row) */
                    const double *row = (const double *)(base + k * rowstep);
                    for (int64_t i = 0; i < m; i++) acc[i] += row[i];
                    continue;
                }
                load_row(x, base + k * rowstep, dt, m);
                if (op == ARR_PROD) for (int64_t i = 0; i < m; i++) acc[i] *= x[i];
                else for (int64_t i = 0; i < m; i++) acc[i] += x[i];
            }
            for (int64_t i = 0; i < m; i++) store(out + i * rz, j->rtype, op == ARR_MEAN ? acc[i] / (double)n : acc[i]);
            break;
        case ARR_VAR: case ARR_STD:
            for (int64_t i = 0; i < m; i++) acc[i] = acc2[i] = 0;
            for (int64_t k = 0; k < n; k++) {
                load_row(x, base + k * rowstep, dt, m);
                for (int64_t i = 0; i < m; i++) acc[i] += x[i];
            }
            for (int64_t i = 0; i < m; i++) acc[i] /= (double)n;
            for (int64_t k = 0; k < n; k++) {
                load_row(x, base + k * rowstep, dt, m);
                for (int64_t i = 0; i < m; i++) {
                    double d = x[i] - acc[i];
                    acc2[i] += d * d;
                }
            }
            for (int64_t i = 0; i < m; i++) {
                double v = n - j->ddof > 0 ? acc2[i] / (double)(n - j->ddof) : NAN;
                store(out + i * rz, j->rtype, op == ARR_STD ? sqrt(v) : v);
            }
            break;
        case ARR_MIN: case ARR_MAX: case ARR_ARGMIN: case ARR_ARGMAX: {
            int want_max = op == ARR_MAX || op == ARR_ARGMAX, arg = op == ARR_ARGMIN || op == ARR_ARGMAX;
            for (int64_t i = 0; i < m; i++) best[i] = 0;
            /* (the first NaN is the answer; without branches, so that the loops vectorize; positions only for
             * argmin and argmax) */
#define RC_PICK(BETTER, A, X)                                                                                  \
    for (int64_t i = 0; i < m; i++) {                                                                          \
        int up = BETTER;                                                                                       \
        A[i] = up ? X[i] : A[i];                                                                               \
        if (arg) best[i] = up ? k : best[i];                                                                   \
    }
            if (flt) {
                load_row(acc, base, dt, m);
                for (int64_t k = 1; k < n; k++) {
                    const double *xr = x;  /* (float64: straight from the row) */
                    if (dt == ARR_F64) xr = (const double *)(base + k * rowstep);
                    else load_row(x, base + k * rowstep, dt, m);
                    if (want_max) RC_PICK((acc[i] == acc[i]) & ((xr[i] != xr[i]) | (xr[i] > acc[i])), acc, xr)
                    else RC_PICK((acc[i] == acc[i]) & ((xr[i] != xr[i]) | (xr[i] < acc[i])), acc, xr)
                }
            } else {
                int64_t *b = (int64_t *)ua;
                load_row_int(b, base, dt, m);
                for (int64_t k = 1; k < n; k++) {
                    load_row_int(xi, base + k * rowstep, dt, m);
                    if (want_max) RC_PICK(xi[i] > b[i], b, xi)
                    else RC_PICK(xi[i] < b[i], b, xi)
                }
            }
#undef RC_PICK
            for (int64_t i = 0; i < m; i++) {
                if (arg) store_int(out + i * rz, j->rtype, best[i]);
                else if (flt) store(out + i * rz, j->rtype, acc[i]);  /* (the very value: exact in its dtype) */
                else store_int(out + i * rz, j->rtype, ((int64_t *)ua)[i]);
            }
            break;
        }
        default: {  /* any, all */
            int any = op == ARR_ANY;
            for (int64_t i = 0; i < m; i++) flag[i] = (unsigned char)!any;
            for (int64_t k = 0; k < n; k++) {
                load_row(x, base + k * rowstep, dt, m);
                if (any) for (int64_t i = 0; i < m; i++) flag[i] |= x[i] != 0;
                else for (int64_t i = 0; i < m; i++) flag[i] &= x[i] != 0;
            }
            memcpy(out, flag, (size_t)m);
            break;
        }
        }
    }
}

/* A reduction of x along axis (ARR_ALL_AXES: of every element), keeping that axis (length 1) when keepdims.
 * ddof: var and std divide by n - ddof. */
static LubeValue reduce_op(LubeVM *vm, int op, LubeValue xv, int axis, int keepdims, int ddof) {
    Array *a;
    LubeValue keep = as_contiguous(vm, xv, -1, &a);
    if (!a) return keep;  /* LCOV_EXCL_LINE: (a copy as big as an Array that is) */
    int64_t outer = 1, n = a->size, inner = 1;
    int nd = 0;
    int64_t shape[ARR_MAXDIM];
    if (axis != ARR_ALL_AXES) {
        n = a->shape[axis];
        for (int d = 0; d < a->ndim; d++) {
            if (d < axis) outer *= a->shape[d];
            if (d > axis) inner *= a->shape[d];
            if (d != axis || keepdims) shape[nd++] = d == axis ? 1 : a->shape[d];
        }
    } else if (keepdims) {
        for (int d = 0; d < a->ndim; d++) shape[nd++] = 1;
    }
    LubeValue r;
    if (n == 0 && op >= ARR_MIN && op <= ARR_ARGMAX) {
        r = lube->error(vm, "%s() of an empty Array: it has no %s", REDUCE_NAMES[op],
                        op == ARR_MIN || op == ARR_ARGMIN ? "smallest" : "largest");
    } else {
        Array *dst;
        int rtype = reduce_dtype(op, a->dtype);
        r = arr_new(vm, rtype, nd, shape, &dst, 0);
        if (dst) {
            ReduceJob j = {op, a->dtype, rtype, ddof, a->data, dst->data, n, inner};
            if (outer * inner == 1) {
                reduce_rows(&j, 0, 1);
            } else if (inner > 1 && n > 0) {  /* (along an axis that is not the last: by rows) */
                int64_t tasks = outer * ((inner + RC_COLS - 1) / RC_COLS);
                par_for(tasks, PARALLEL_WORK / (n * RC_COLS) + 1, reduce_cols, &j);
            } else {
                par_for(outer * inner, PARALLEL_WORK / (n ? n : 1) + 1, reduce_rows, &j);
            }
        }
    }
    lube->release(keep);
    return r;
}

/* cumsum and cumprod: running totals along axis (ARR_ALL_AXES: over the flattened Array). */
static LubeValue cumulate(LubeVM *vm, int prod, LubeValue xv, int axis) {
    Array *a;
    LubeValue keep = as_contiguous(vm, xv, -1, &a);
    if (!a) return keep;  /* LCOV_EXCL_LINE */
    int64_t outer = 1, n = a->size, inner = 1;
    int64_t flat = a->size;
    int rtype = IS_FLOAT(a->dtype) ? a->dtype : ARR_I64;
    Array *dst;
    LubeValue r = axis == ARR_ALL_AXES ? arr_new(vm, rtype, 1, &flat, &dst, 0) : arr_new(vm, rtype, a->ndim, a->shape, &dst, 0);
    if (axis != ARR_ALL_AXES) {
        n = a->shape[axis];
        for (int d = 0; d < a->ndim; d++) {
            if (d < axis) outer *= a->shape[d];
            if (d > axis) inner *= a->shape[d];
        }
    }
    int z = ITEMSIZE[a->dtype], rz = ITEMSIZE[rtype];
    for (int64_t o = 0; o < outer; o++)
        for (int64_t i = 0; i < inner; i++) {
            double t = prod;
            int64_t ti = prod;
            for (int64_t k = 0; k < n; k++) {
                int64_t at = (o * n + k) * inner + i;
                if (IS_FLOAT(rtype)) {
                    double x = load(a->data + at * z, a->dtype);
                    t = prod ? t * x : t + x;
                    store(dst->data + at * rz, rtype, t);
                } else {
                    int64_t x = load_int(a->data + at * z, a->dtype);
                    ti = prod ? (int64_t)((uint64_t)ti * (uint64_t)x) : (int64_t)((uint64_t)ti + (uint64_t)x);
                    store_int(dst->data + at * rz, rtype, ti);
                }
            }
        }
    lube->release(keep);
    return r;
}

/* sum_to: x summed over the axes broadcasting added or stretched, down to shape (gradients flow back so). */
static LubeValue sum_to(LubeVM *vm, LubeValue xv, int nd, const int64_t *shape) {
    LubeValue cur = lube->retain(xv);
    Array *a = arr_of(cur);
    while (a->ndim > nd) {  /* axes broadcasting added in front */
        LubeValue next = reduce_op(vm, ARR_SUM, cur, 0, 0, 0);
        lube->release(cur);
        cur = next;
        a = arr_of(cur);
    }
    for (int d = 0; d < nd; d++)
        if (shape[d] == 1 && a->shape[d] != 1) {
            LubeValue next = reduce_op(vm, ARR_SUM, cur, d, 1, 0);
            lube->release(cur);
            cur = next;
            a = arr_of(cur);
        }
    return cur;
}
