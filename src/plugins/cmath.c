/*
 * cmath.c - the plugin cmath: Python's cmath module, and complex numbers.
 *
 *   import cmath.*                       (or import cmath: cmath.Complex(3, 4), cmath.sqrt(-1))
 *   let z = Complex(3, 4)                also Complex(2) (no imaginary part), Complex("1+2j"), Complex()
 *   z.real, z.imag, z.conjugate()        it prints as Python prints it: (3+4j), 2j, (1-0j)
 *   z + 1, 2 * z, z - w, z / w, z ** 0.5, -z, z == w, z != 1
 *   sqrt(-1), exp, log(z, base?), log10, sin, cos, tan, asin, acos, atan,
 *   sinh, cosh, tanh, asinh, acosh, atanh   (of a Number or a Complex: a Complex)
 *   phase(z), polar(z) ([r, phi]), rect(r, phi), abs(z)
 *   isfinite, isinf, isnan, isclose(a, b, {rel_tol: 1e-9, abs_tol: 0})
 *   pi, e, tau, inf, nan, infj(), nanj()
 *
 * The results are Python's, by Python's own algorithms (Kahan's branch cuts,
 * and C99's answers for inf and nan), so they agree with Python's to the last
 * bit where the C library's exp, log, sin... do. Arithmetic is Python's too:
 * a Number with a Complex is a real number, not a Complex with 0 for its
 * imaginary part (2 * Complex(inf, 1) is (inf+2j), not nan).
 *
 * Where Python raises an error, so does this: log(0), atanh(1), exp(1000)
 * (too large), Complex(1) / 0. `import cmath.*` also gives `abs`, which takes
 * Numbers too (as Lube's own abs does, which it replaces).
 *
 * Python's cmath.infj and cmath.nanj are Complex, and a package's constants
 * are Numbers: here they are functions, infj() and nanj().
 */
#include "../lube_plugin.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
/* Every operation rounded on its own, as written (gcc never fuses them under -std=c99): the same
 * results on every system. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "f_sqrt" -> "sqrt" */

static const LubeAPI *lube;
static LubeType *ComplexType;

typedef struct {
    double re, im;
} Cx;

enum { C_OK, C_DOMAIN, C_RANGE };  /* how a function went: as Python's errno, EDOM or ERANGE */

static const double PI = 3.141592653589793238462643383279502884;
static const double LN2 = 0.6931471805599453094;
static const double LN10 = 2.302585092994045684;
static const double E = 2.718281828459045235360287471352662498;

/* ======================================================================
 * Complex numbers as values
 * ==================================================================== */

static Cx cx(double re, double im) {
    Cx z = {re, im};
    return z;
}

static LubeValue new_complex(LubeVM *vm, Cx z) {
    void *data;
    LubeValue v = lube->new_foreign(vm, ComplexType, &data);
    *(Cx *)data = z;
    return v;
}

/* v as a complex number: a Complex, or a Number (with 0 for its imaginary part). 0 if it is neither. */
static int complex_of(LubeValue v, Cx *out) {
    const Cx *z = (const Cx *)lube->foreign(v, ComplexType);
    if (z) {
        *out = *z;
        return 1;
    }
    if (lube->kind(v) != LUBE_NUMBER) return 0;
    *out = cx(lube->number(v), 0.0);
    return 1;
}

/* Argument i as a complex number, or 0 with the error in *err. */
static int arg_of(LubeVM *vm, const char *fn, const LubeValue *argv, int i, Cx *out, LubeValue *err) {
    if (complex_of(argv[i], out)) return 1;
    *err = lube->error(vm, "%s() expects a Number or a Complex as argument %d, got %s", fn, i + 1,
                       lube->type_name(argv[i]));
    return 0;
}

/* A part of a complex number as it prints: Lube's way, but -0 shows its sign (as Python's does). */
static void part(double x, char *buf, size_t size) {
    if (x == 0 && signbit(x)) snprintf(buf, size, "-0");
    else lube->format_number(x, buf, size);
}

/* As Python prints a complex: (1+2j), 2j (no real part, or +0), (-0-1j), (nan+infj) */
static int describe(const void *data, char *buf, size_t size) {
    const Cx *z = (const Cx *)data;
    char re[32], im[32];
    part(z->im, im, sizeof im);
    if (z->re == 0 && !signbit(z->re)) return snprintf(buf, size, "%sj", im);
    part(z->re, re, sizeof re);
    return snprintf(buf, size, "(%s%s%sj)", re, im[0] == '-' ? "" : "+", im);
}

/* An argument as it prints, for messages: a Number's way, or a Complex's. */
static void text_of(LubeValue v, char *buf, size_t size) {
    const Cx *z = (const Cx *)lube->foreign(v, ComplexType);
    if (z) describe(z, buf, size);
    else lube->format_number(lube->number(v), buf, size);
}

/* ======================================================================
 * Arithmetic, as Python's (Objects/complexobject.c)
 * ==================================================================== */

static Cx c_prod(Cx z, Cx w) {
    double a = z.re, b = z.im, c = w.re, d = w.im;
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    Cx r = cx(ac - bd, ad + bc);
    if (isnan(r.re) && isnan(r.im)) {  /* infinities that came out as nan + nanj: C11's Annex G */
        int recalc = 0;
        if (isinf(a) || isinf(b)) {  /* z is infinite: "box" it, and nans in w become 0 */
            a = copysign(isinf(a) ? 1.0 : 0.0, a);
            b = copysign(isinf(b) ? 1.0 : 0.0, b);
            if (isnan(c)) c = copysign(0.0, c);
            if (isnan(d)) d = copysign(0.0, d);
            recalc = 1;
        }
        if (isinf(c) || isinf(d)) {  /* w is */
            c = copysign(isinf(c) ? 1.0 : 0.0, c);
            d = copysign(isinf(d) ? 1.0 : 0.0, d);
            if (isnan(a)) a = copysign(0.0, a);
            if (isnan(b)) b = copysign(0.0, b);
            recalc = 1;
        }
        if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {  /* an overflow */
            if (isnan(a)) a = copysign(0.0, a);
            if (isnan(b)) b = copysign(0.0, b);
            if (isnan(c)) c = copysign(0.0, c);
            if (isnan(d)) d = copysign(0.0, d);
            recalc = 1;
        }
        if (recalc) r = cx(INFINITY * (a * c - b * d), INFINITY * (a * d + b * c));
    }
    return r;
}

/* z / w, dividing by whichever of w's parts is larger (Smith's method); *err C_DOMAIN when w is 0 */
static Cx c_quot(Cx a, Cx b, int *err) {
    Cx r;
    double abs_re = fabs(b.re), abs_im = fabs(b.im);
    if (abs_re >= abs_im) {
        if (abs_re == 0.0) {
            *err = C_DOMAIN;
            return cx(0.0, 0.0);
        }
        double ratio = b.im / b.re, denom = b.re + b.im * ratio;
        r = cx((a.re + a.im * ratio) / denom, (a.im - a.re * ratio) / denom);
    } else if (abs_im >= abs_re) {
        double ratio = b.re / b.im, denom = b.re * ratio + b.im;
        r = cx((a.re * ratio + a.im) / denom, (a.im * ratio - a.re) / denom);
    } else {  /* a part of b is nan */
        r = cx(NAN, NAN);
    }
    if (isnan(r.re) && isnan(r.im)) {  /* infinities and zeros that came out as nan + nanj */
        if ((isinf(a.re) || isinf(a.im)) && isfinite(b.re) && isfinite(b.im)) {
            double x = copysign(isinf(a.re) ? 1.0 : 0.0, a.re), y = copysign(isinf(a.im) ? 1.0 : 0.0, a.im);
            r = cx(INFINITY * (x * b.re + y * b.im), INFINITY * (y * b.re - x * b.im));
        } else if ((isinf(abs_re) || isinf(abs_im)) && isfinite(a.re) && isfinite(a.im)) {
            double x = copysign(isinf(b.re) ? 1.0 : 0.0, b.re), y = copysign(isinf(b.im) ? 1.0 : 0.0, b.im);
            r = cx(0.0 * (a.re * x + a.im * y), 0.0 * (a.im * x - a.re * y));
        }
    }
    return r;
}

/* x / w for a real x */
static Cx rc_quot(double a, Cx b, int *err) {
    Cx r;
    double abs_re = fabs(b.re), abs_im = fabs(b.im);
    if (abs_re >= abs_im) {
        if (abs_re == 0.0) {
            *err = C_DOMAIN;
            return cx(0.0, 0.0);
        }
        double ratio = b.im / b.re, denom = b.re + b.im * ratio;
        r = cx(a / denom, (-a * ratio) / denom);
    } else if (abs_im >= abs_re) {
        double ratio = b.re / b.im, denom = b.re * ratio + b.im;
        r = cx((a * ratio) / denom, (-a) / denom);
    } else {
        r = cx(NAN, NAN);
    }
    if (isnan(r.re) && isnan(r.im) && isfinite(a) && (isinf(abs_re) || isinf(abs_im))) {
        double x = copysign(isinf(b.re) ? 1.0 : 0.0, b.re), y = copysign(isinf(b.im) ? 1.0 : 0.0, b.im);
        r = cx(0.0 * (a * x), 0.0 * (-a * y));
    }
    return r;
}

/* |z|: inf if a part is, even with a nan */
static double c_abs(Cx z) {
    if (!isfinite(z.re) || !isfinite(z.im)) {
        if (isinf(z.re)) return fabs(z.re);
        if (isinf(z.im)) return fabs(z.im);
        return NAN;
    }
    return hypot(z.re, z.im);
}

/* z ** n for a whole n from 0 to 100, by squaring (in Python 3.15's order, which decides the signs of 0) */
static Cx c_powu(Cx x, long n) {
    Cx r = cx(1.0, 0.0), p = x;
    for (long mask = 1; n >= mask; mask <<= 1) {
        if (n & mask) r = c_prod(r, p);
        p = c_prod(p, p);
    }
    return r;
}

/* a ** b: *err C_DOMAIN for 0 to a negative or complex power, C_RANGE when a part is inf (as Python 3.15
 * has it, even when a or b is) */
static Cx c_pow(Cx a, Cx b, int *err) {
    Cx r;
    if (b.im == 0.0 && b.re == floor(b.re) && fabs(b.re) <= 100) {  /* a small whole power: exact, and faster */
        long n = (long)b.re;
        r = n > 0 ? c_powu(a, n) : c_quot(cx(1.0, 0.0), c_powu(a, -n), err);
    } else if (a.re == 0.0 && a.im == 0.0) {
        if (b.im != 0.0 || b.re < 0.0) *err = C_DOMAIN;
        return cx(0.0, 0.0);
    } else {
        double vabs = hypot(a.re, a.im), len = pow(vabs, b.re), at = atan2(a.im, a.re), phase = at * b.re;
        if (b.im != 0.0) {
            len *= exp(-at * b.im);
            phase += b.im * log(vabs);
        }
        r = cx(len * cos(phase), len * sin(phase));
    }
    if ((isinf(r.re) || isinf(r.im)) && *err == C_OK) *err = C_RANGE;
    return r;
}

/* ======================================================================
 * The functions, as Python's (Modules/cmathmodule.c): Kahan's branch cuts,
 * and the answers C99's Annex G gives for infinities and nans, from tables
 * ==================================================================== */

#define LARGE (DBL_MAX / 4.)            /* past this, the formulas would overflow */
#define SQRT_LARGE (sqrt(LARGE))
#define LOG_LARGE (log(LARGE))
#define SQRT_DBL_MIN (sqrt(DBL_MIN))
#define SCALE_UP (2 * (DBL_MANT_DIG / 2) + 1)  /* (an odd power of 2 that makes a subnormal normal) */
#define SCALE_DOWN (-(SCALE_UP + 1) / 2)

/* What a part is, for the tables: -inf, below 0, -0, +0, above 0, inf, nan */
enum { ST_NINF, ST_NEG, ST_NZERO, ST_PZERO, ST_POS, ST_PINF, ST_NAN };

static int special_type(double d) {
    if (isfinite(d)) {
        if (d != 0) return signbit(d) ? ST_NEG : ST_POS;
        return signbit(d) ? ST_NZERO : ST_PZERO;
    }
    if (isnan(d)) return ST_NAN;
    return signbit(d) ? ST_NINF : ST_PINF;
}

#define P 3.141592653589793238462643383279502884
#define P14 (0.25 * P)
#define P12 (0.5 * P)
#define P34 (0.75 * P)
#define INF INFINITY
#define N NAN
#define U -9.5426319407711027e33 /* (never used: a finite number, which the formulas take) */

/* A part that is inf or nan: the table's answer (and no error) */
#define SPECIAL_VALUE(z, table)                     \
    if (!isfinite((z).re) || !isfinite((z).im)) {   \
        return table[special_type((z).re)][special_type((z).im)]; \
    }

static const Cx acos_special[7][7] = {
    {{P34, INF}, {P, INF}, {P, INF}, {P, -INF}, {P, -INF}, {P34, -INF}, {N, INF}},
    {{P12, INF}, {U, U}, {U, U}, {U, U}, {U, U}, {P12, -INF}, {N, N}},
    {{P12, INF}, {U, U}, {P12, 0.}, {P12, -0.}, {U, U}, {P12, -INF}, {P12, N}},
    {{P12, INF}, {U, U}, {P12, 0.}, {P12, -0.}, {U, U}, {P12, -INF}, {P12, N}},
    {{P12, INF}, {U, U}, {U, U}, {U, U}, {U, U}, {P12, -INF}, {N, N}},
    {{P14, INF}, {0., INF}, {0., INF}, {0., -INF}, {0., -INF}, {P14, -INF}, {N, INF}},
    {{N, INF}, {N, N}, {N, N}, {N, N}, {N, N}, {N, -INF}, {N, N}}};

static const Cx acosh_special[7][7] = {
    {{INF, -P34}, {INF, -P}, {INF, -P}, {INF, P}, {INF, P}, {INF, P34}, {INF, N}},
    {{INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {0., -P12}, {0., P12}, {U, U}, {INF, P12}, {N, P12}},
    {{INF, -P12}, {U, U}, {0., -P12}, {0., P12}, {U, U}, {INF, P12}, {N, P12}},
    {{INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P14}, {INF, -0.}, {INF, -0.}, {INF, 0.}, {INF, 0.}, {INF, P14}, {INF, N}},
    {{INF, N}, {N, N}, {N, N}, {N, N}, {N, N}, {INF, N}, {N, N}}};

static const Cx asinh_special[7][7] = {
    {{-INF, -P14}, {-INF, -0.}, {-INF, -0.}, {-INF, 0.}, {-INF, 0.}, {-INF, P14}, {-INF, N}},
    {{-INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {-INF, P12}, {N, N}},
    {{-INF, -P12}, {U, U}, {-0., -0.}, {-0., 0.}, {U, U}, {-INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P14}, {INF, -0.}, {INF, -0.}, {INF, 0.}, {INF, 0.}, {INF, P14}, {INF, N}},
    {{INF, N}, {N, N}, {N, -0.}, {N, 0.}, {N, N}, {INF, N}, {N, N}}};

static const Cx atanh_special[7][7] = {
    {{-0., -P12}, {-0., -P12}, {-0., -P12}, {-0., P12}, {-0., P12}, {-0., P12}, {-0., N}},
    {{-0., -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {-0., P12}, {N, N}},
    {{-0., -P12}, {U, U}, {-0., -0.}, {-0., 0.}, {U, U}, {-0., P12}, {-0., N}},
    {{0., -P12}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {0., P12}, {0., N}},
    {{0., -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {0., P12}, {N, N}},
    {{0., -P12}, {0., -P12}, {0., -P12}, {0., P12}, {0., P12}, {0., P12}, {0., N}},
    {{0., -P12}, {N, N}, {N, N}, {N, N}, {N, N}, {0., P12}, {N, N}}};

static const Cx cosh_special[7][7] = {
    {{INF, N}, {U, U}, {INF, 0.}, {INF, -0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{N, 0.}, {U, U}, {1., 0.}, {1., -0.}, {U, U}, {N, 0.}, {N, 0.}},
    {{N, 0.}, {U, U}, {1., -0.}, {1., 0.}, {U, U}, {N, 0.}, {N, 0.}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{INF, N}, {U, U}, {INF, -0.}, {INF, 0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {N, N}, {N, 0.}, {N, 0.}, {N, N}, {N, N}, {N, N}}};

static const Cx exp_special[7][7] = {
    {{0., 0.}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {0., 0.}, {0., 0.}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{N, N}, {U, U}, {1., -0.}, {1., 0.}, {U, U}, {N, N}, {N, N}},
    {{N, N}, {U, U}, {1., -0.}, {1., 0.}, {U, U}, {N, N}, {N, N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{INF, N}, {U, U}, {INF, -0.}, {INF, 0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {N, N}, {N, -0.}, {N, 0.}, {N, N}, {N, N}, {N, N}}};

static const Cx log_special[7][7] = {
    {{INF, -P34}, {INF, -P}, {INF, -P}, {INF, P}, {INF, P}, {INF, P34}, {INF, N}},
    {{INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {-INF, -P}, {-INF, P}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {-INF, -0.}, {-INF, 0.}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P12}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, P12}, {N, N}},
    {{INF, -P14}, {INF, -0.}, {INF, -0.}, {INF, 0.}, {INF, 0.}, {INF, P14}, {INF, N}},
    {{INF, N}, {N, N}, {N, N}, {N, N}, {N, N}, {INF, N}, {N, N}}};

static const Cx sinh_special[7][7] = {
    {{INF, N}, {U, U}, {-INF, -0.}, {-INF, 0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{0., N}, {U, U}, {-0., -0.}, {-0., 0.}, {U, U}, {0., N}, {0., N}},
    {{0., N}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {0., N}, {0., N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{INF, N}, {U, U}, {INF, -0.}, {INF, 0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {N, N}, {N, -0.}, {N, 0.}, {N, N}, {N, N}, {N, N}}};

static const Cx sqrt_special[7][7] = {
    {{INF, -INF}, {0., -INF}, {0., -INF}, {0., INF}, {0., INF}, {INF, INF}, {N, INF}},
    {{INF, -INF}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, INF}, {N, N}},
    {{INF, -INF}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {INF, INF}, {N, N}},
    {{INF, -INF}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {INF, INF}, {N, N}},
    {{INF, -INF}, {U, U}, {U, U}, {U, U}, {U, U}, {INF, INF}, {N, N}},
    {{INF, -INF}, {INF, -0.}, {INF, -0.}, {INF, 0.}, {INF, 0.}, {INF, INF}, {INF, N}},
    {{INF, -INF}, {N, N}, {N, N}, {N, N}, {N, N}, {INF, INF}, {N, N}}};

static const Cx tanh_special[7][7] = {
    {{-1., 0.}, {U, U}, {-1., -0.}, {-1., 0.}, {U, U}, {-1., 0.}, {-1., 0.}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{-0.0, N}, {U, U}, {-0., -0.}, {-0., 0.}, {U, U}, {-0.0, N}, {-0., N}},
    {{0.0, N}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {0.0, N}, {0., N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{1., 0.}, {U, U}, {1., -0.}, {1., 0.}, {U, U}, {1., 0.}, {1., 0.}},
    {{N, N}, {N, N}, {N, -0.}, {N, 0.}, {N, N}, {N, N}, {N, N}}};

static const Cx rect_special[7][7] = {
    {{INF, N}, {U, U}, {-INF, 0.}, {-INF, -0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{0., 0.}, {U, U}, {-0., 0.}, {-0., -0.}, {U, U}, {0., 0.}, {0., 0.}},
    {{0., 0.}, {U, U}, {0., -0.}, {0., 0.}, {U, U}, {0., 0.}, {0., 0.}},
    {{N, N}, {U, U}, {U, U}, {U, U}, {U, U}, {N, N}, {N, N}},
    {{INF, N}, {U, U}, {INF, -0.}, {INF, 0.}, {U, U}, {INF, N}, {INF, N}},
    {{N, N}, {N, N}, {N, 0.}, {N, 0.}, {N, N}, {N, N}, {N, N}}};

#undef P
#undef P14
#undef P12
#undef P34
#undef INF
#undef N
#undef U

static double log1p_of(double x) { return x == 0 ? x : log1p(x); }  /* (log1p(-0) is -0 everywhere) */

static Cx c_sqrt(Cx z, int *err) {
    (void)err;
    SPECIAL_VALUE(z, sqrt_special);
    if (z.re == 0 && z.im == 0) return cx(0.0, z.im);
    double ax = fabs(z.re), ay = fabs(z.im), s, d;
    if (ax < DBL_MIN && ay < DBL_MIN) {  /* subnormal: scaled up first */
        ax = ldexp(ax, SCALE_UP);
        s = ldexp(sqrt(ax + hypot(ax, ldexp(ay, SCALE_UP))), SCALE_DOWN);
    } else {
        ax /= 8.;
        s = 2. * sqrt(ax + hypot(ax, ay / 8.));
    }
    d = ay / (2. * s);
    return z.re >= 0 ? cx(s, copysign(d, z.im)) : cx(d, copysign(s, z.im));
}

static Cx c_acos(Cx z, int *err) {
    SPECIAL_VALUE(z, acos_special);
    if (fabs(z.re) > LARGE || fabs(z.im) > LARGE)  /* (no needless overflow) */
        return cx(atan2(fabs(z.im), z.re), -copysign(log(hypot(z.re / 2., z.im / 2.)) + LN2 * 2., z.im));
    Cx s1 = c_sqrt(cx(1. - z.re, -z.im), err), s2 = c_sqrt(cx(1. + z.re, z.im), err);
    return cx(2. * atan2(s1.re, s2.re), asinh(s2.re * s1.im - s2.im * s1.re));
}

static Cx c_acosh(Cx z, int *err) {
    SPECIAL_VALUE(z, acosh_special);
    if (fabs(z.re) > LARGE || fabs(z.im) > LARGE)
        return cx(log(hypot(z.re / 2., z.im / 2.)) + LN2 * 2., atan2(z.im, z.re));
    Cx s1 = c_sqrt(cx(z.re - 1., z.im), err), s2 = c_sqrt(cx(z.re + 1., z.im), err);
    return cx(asinh(s1.re * s2.re + s1.im * s2.im), 2. * atan2(s1.im, s2.re));
}

static Cx c_asinh(Cx z, int *err) {
    SPECIAL_VALUE(z, asinh_special);
    if (fabs(z.re) > LARGE || fabs(z.im) > LARGE) {
        double l = log(hypot(z.re / 2., z.im / 2.)) + LN2 * 2.;
        return cx(z.im >= 0 ? copysign(l, z.re) : -copysign(l, -z.re), atan2(z.im, fabs(z.re)));
    }
    Cx s1 = c_sqrt(cx(1. + z.im, -z.re), err), s2 = c_sqrt(cx(1. - z.im, z.re), err);
    return cx(asinh(s1.re * s2.im - s2.re * s1.im), atan2(z.im, s1.re * s2.re - s1.im * s2.im));
}

static Cx c_asin(Cx z, int *err) {  /* asin(z) = -i asinh(iz) */
    Cx s = c_asinh(cx(-z.im, z.re), err);
    return cx(s.im, -s.re);
}

static Cx c_atanh(Cx z, int *err) {
    SPECIAL_VALUE(z, atanh_special);
    if (z.re < 0) {  /* atanh(z) = -atanh(-z) */
        Cx r = c_atanh(cx(-z.re, -z.im), err);
        return cx(-r.re, -r.im);
    }
    double ay = fabs(z.im);
    if (z.re > SQRT_LARGE || ay > SQRT_LARGE) {  /* large: 1/z, and pi/2 */
        double h = hypot(z.re / 2., z.im / 2.);
        return cx(z.re / 4. / h / h, copysign(PI / 2., z.im));
    }
    if (z.re == 1. && ay < SQRT_DBL_MIN) {
        if (ay == 0) {  /* atanh(1) is inf */
            *err = C_DOMAIN;
            return cx(INFINITY, z.im);
        }
        return cx(-log(sqrt(ay) / sqrt(hypot(ay, 2.))), copysign(atan2(2., -ay) / 2, z.im));
    }
    return cx(log1p_of(4. * z.re / ((1 - z.re) * (1 - z.re) + ay * ay)) / 4.,
              -atan2(-2. * z.im, (1 - z.re) * (1 + z.re) - ay * ay) / 2.);
}

static Cx c_atan(Cx z, int *err) {  /* atan(z) = -i atanh(iz) */
    Cx s = c_atanh(cx(-z.im, z.re), err);
    return cx(s.im, -s.re);
}

/* cosh, sinh: inf + iy, for a finite y other than 0, by the signs of cos y and sin y */
static Cx c_cosh(Cx z, int *err) {
    Cx r;
    if (!isfinite(z.re) || !isfinite(z.im)) {
        if (isinf(z.re) && isfinite(z.im) && z.im != 0) {
            r = cx(copysign(INFINITY, cos(z.im)), copysign(INFINITY, sin(z.im)));
            if (z.re < 0) r.im = -r.im;
        } else {
            r = cosh_special[special_type(z.re)][special_type(z.im)];
        }
        if (isinf(z.im) && !isnan(z.re)) *err = C_DOMAIN;
        return r;
    }
    if (fabs(z.re) > LOG_LARGE) {  /* cosh(x) would overflow, but cosh(z) need not */
        double x_minus_one = z.re - copysign(1., z.re);
        r = cx(cos(z.im) * cosh(x_minus_one) * E, sin(z.im) * sinh(x_minus_one) * E);
    } else {
        r = cx(cos(z.im) * cosh(z.re), sin(z.im) * sinh(z.re));
    }
    if (isinf(r.re) || isinf(r.im)) *err = C_RANGE;
    return r;
}

static Cx c_sinh(Cx z, int *err) {
    Cx r;
    if (!isfinite(z.re) || !isfinite(z.im)) {
        if (isinf(z.re) && isfinite(z.im) && z.im != 0) {
            r = cx(copysign(INFINITY, cos(z.im)), copysign(INFINITY, sin(z.im)));
            if (z.re < 0) r.re = -r.re;
        } else {
            r = sinh_special[special_type(z.re)][special_type(z.im)];
        }
        if (isinf(z.im) && !isnan(z.re)) *err = C_DOMAIN;
        return r;
    }
    if (fabs(z.re) > LOG_LARGE) {
        double x_minus_one = z.re - copysign(1., z.re);
        r = cx(cos(z.im) * sinh(x_minus_one) * E, sin(z.im) * cosh(x_minus_one) * E);
    } else {
        r = cx(cos(z.im) * sinh(z.re), sin(z.im) * cosh(z.re));
    }
    if (isinf(r.re) || isinf(r.im)) *err = C_RANGE;
    return r;
}

static Cx c_cos(Cx z, int *err) { return c_cosh(cx(-z.im, z.re), err); }  /* cos(z) = cosh(iz) */

static Cx c_sin(Cx z, int *err) {  /* sin(z) = -i sinh(iz) */
    Cx s = c_sinh(cx(-z.im, z.re), err);
    return cx(s.im, -s.re);
}

static Cx c_exp(Cx z, int *err) {
    Cx r;
    if (!isfinite(z.re) || !isfinite(z.im)) {
        if (isinf(z.re) && isfinite(z.im) && z.im != 0) {
            if (z.re > 0) r = cx(copysign(INFINITY, cos(z.im)), copysign(INFINITY, sin(z.im)));
            else r = cx(copysign(0., cos(z.im)), copysign(0., sin(z.im)));
        } else {
            r = exp_special[special_type(z.re)][special_type(z.im)];
        }
        if (isinf(z.im) && (isfinite(z.re) || (isinf(z.re) && z.re > 0))) *err = C_DOMAIN;
        return r;
    }
    if (z.re > LOG_LARGE) {
        double l = exp(z.re - 1.);
        r = cx(l * cos(z.im) * E, l * sin(z.im) * E);
    } else {
        double l = exp(z.re);
        r = cx(l * cos(z.im), l * sin(z.im));
    }
    if (isinf(r.re) || isinf(r.im)) *err = C_RANGE;
    return r;
}

static Cx c_log(Cx z, int *err) {
    SPECIAL_VALUE(z, log_special);
    double ax = fabs(z.re), ay = fabs(z.im), re;
    if (ax > LARGE || ay > LARGE) {
        re = log(hypot(ax / 2., ay / 2.)) + LN2;
    } else if (ax < DBL_MIN && ay < DBL_MIN) {
        if (ax > 0 || ay > 0) {  /* |z| subnormal: scaled up first */
            re = log(hypot(ldexp(ax, DBL_MANT_DIG), ldexp(ay, DBL_MANT_DIG))) - DBL_MANT_DIG * LN2;
        } else {  /* log(0) */
            *err = C_DOMAIN;
            return cx(-INFINITY, atan2(z.im, z.re));
        }
    } else {
        double h = hypot(ax, ay);
        if (0.71 <= h && h <= 1.73) {  /* |z| near 1: log1p keeps the digits */
            double am = ax > ay ? ax : ay, an = ax > ay ? ay : ax;
            re = log1p_of((am - 1) * (am + 1) + an * an) / 2.;
        } else {
            re = log(h);
        }
    }
    return cx(re, atan2(z.im, z.re));
}

static Cx c_log10(Cx z, int *err) {
    Cx r = c_log(z, err);
    return cx(r.re / LN10, r.im / LN10);
}

static Cx c_tanh(Cx z, int *err) {
    if (!isfinite(z.re) || !isfinite(z.im)) {
        Cx r;
        if (isinf(z.re) && isfinite(z.im) && z.im != 0)
            r = cx(z.re > 0 ? 1.0 : -1.0, copysign(0., 2. * sin(z.im) * cos(z.im)));
        else
            r = tanh_special[special_type(z.re)][special_type(z.im)];
        if (isinf(z.im) && isfinite(z.re)) *err = C_DOMAIN;
        return r;
    }
    if (fabs(z.re) > LOG_LARGE)  /* (2 * z.im could overflow) */
        return cx(copysign(1., z.re), 4. * sin(z.im) * cos(z.im) * exp(-2. * fabs(z.re)));
    double tx = tanh(z.re), ty = tan(z.im), c = 1. / cosh(z.re), txty = tx * ty, denom = 1. + txty * txty;
    return cx(tx * (1. + ty * ty) / denom, ((ty / denom) * c) * c);
}

static Cx c_tan(Cx z, int *err) {  /* tan(z) = -i tanh(iz) */
    Cx s = c_tanh(cx(-z.im, z.re), err);
    return cx(s.im, -s.re);
}

/* ======================================================================
 * The package's functions
 * ==================================================================== */

/* A function's answer: the Complex, or the error its trouble calls for */
static LubeValue answer(LubeVM *vm, const char *fn, LubeValue arg, Cx r, int err) {
    if (err == C_OK) return new_complex(vm, r);
    char text[80];
    text_of(arg, text, sizeof text);
    if (err == C_DOMAIN) return lube->error(vm, "%s() is not defined at %s", fn, text);
    size_t n = strlen(text);
    int bare = text[0] == '(';  /* exp(1+2j), not exp((1+2j)) */
    return lube->error(vm, "%s(%.*s): the result is too large for a Complex", fn, (int)(n - 2 * bare), text + bare);
}

#define COMPLEX_FN(name)                                    \
    FN(f_##name) {                                          \
        Cx z;                                               \
        LubeValue e;                                        \
        if (!arg_of(vm, #name, argv, 0, &z, &e)) return e;  \
        int err = C_OK;                                     \
        Cx r = c_##name(z, &err);                           \
        return answer(vm, #name, argv[0], r, err);          \
    }
COMPLEX_FN(sqrt)
COMPLEX_FN(exp)
COMPLEX_FN(log10)
COMPLEX_FN(sin)
COMPLEX_FN(cos)
COMPLEX_FN(tan)
COMPLEX_FN(asin)
COMPLEX_FN(acos)
COMPLEX_FN(atan)
COMPLEX_FN(sinh)
COMPLEX_FN(cosh)
COMPLEX_FN(tanh)
COMPLEX_FN(asinh)
COMPLEX_FN(acosh)
COMPLEX_FN(atanh)

/* log(z), log(z, base): as Python's, whose log(0, base) is -inf + nanj, not an error (only the base's
 * log and the division can fail) */
FN(f_log) {
    Cx z, b;
    LubeValue e;
    if (!arg_of(vm, "log", argv, 0, &z, &e)) return e;
    int err = C_OK;
    Cx r = c_log(z, &err);
    if (argc < 2) return answer(vm, "log", argv[0], r, err);
    if (!arg_of(vm, "log", argv, 1, &b, &e)) return e;
    char text[80];
    err = C_OK;
    Cx lb = c_log(b, &err);
    if (err != C_OK) {
        text_of(argv[1], text, sizeof text);
        return lube->error(vm, "log() is not defined at base %s", text);
    }
    r = c_quot(r, lb, &err);
    if (err != C_OK) return lube->error(vm, "log() is not defined for base 1 (log(1) is 0)");
    return new_complex(vm, r);
}

/* phase(z): the angle of z, from -pi to pi */
FN(f_phase) {
    Cx z;
    LubeValue e;
    if (!arg_of(vm, "phase", argv, 0, &z, &e)) return e;
    return lube->number_value(atan2(z.im, z.re));
}

/* The error for |z| too large for a Number */
static LubeValue abs_too_large(LubeVM *vm, const char *fn, LubeValue arg) {
    char text[80];
    text_of(arg, text, sizeof text);
    return lube->error(vm, "%s(): the absolute value of %s is too large for a Number", fn, text);
}

/* polar(z): [|z|, phase(z)] */
FN(f_polar) {
    Cx z;
    LubeValue e;
    if (!arg_of(vm, "polar", argv, 0, &z, &e)) return e;
    double r = c_abs(z);
    if (isinf(r) && isfinite(z.re) && isfinite(z.im)) return abs_too_large(vm, "polar", argv[0]);
    LubeValue l = lube->new_list(vm);
    lube->list_push(l, lube->number_value(r));
    lube->list_push(l, lube->number_value(atan2(z.im, z.re)));
    return l;
}

/* rect(r, phi): the Complex of length r at angle phi */
FN(f_rect) {
    for (int i = 0; i < 2; i++)
        if (lube->kind(argv[i]) != LUBE_NUMBER)
            return lube->error(vm, "rect() expects a Number as argument %d, got %s", i + 1, lube->type_name(argv[i]));
    double r = lube->number(argv[0]), phi = lube->number(argv[1]);
    Cx z;
    if (!isfinite(r) || !isfinite(phi)) {
        if (isinf(r) && isfinite(phi) && phi != 0) {  /* inf at an angle: the signs of cos and sin */
            z = cx(copysign(INFINITY, cos(phi)), copysign(INFINITY, sin(phi)));
            if (r < 0) z = cx(-z.re, -z.im);
        } else {
            z = rect_special[special_type(r)][special_type(phi)];
        }
        if (r != 0 && !isnan(r) && isinf(phi)) {
            char num[32];
            lube->format_number(r, num, sizeof num);
            return lube->error(vm, "rect(%s, %s): an infinite angle has no direction", num, phi > 0 ? "inf" : "-inf");
        }
    } else if (phi == 0) {
        z = cx(r, r * phi);  /* (exact, and the sign of 0 as Python's) */
    } else {
        z = cx(r * cos(phi), r * sin(phi));
    }
    return new_complex(vm, z);
}

/* abs(z): |z|, a Number; abs(x) for a Number, as Lube's own abs */
FN(f_abs) {
    Cx z;
    LubeValue e;
    if (lube->kind(argv[0]) == LUBE_NUMBER) {
        double x = lube->number(argv[0]);
        return x > 0 || (x == 0 && !signbit(x)) ? lube->retain(argv[0]) : lube->number_value(fabs(x));  /* (a sized integer stays one) */
    }
    if (!arg_of(vm, "abs", argv, 0, &z, &e)) return e;
    double r = c_abs(z);
    if (isinf(r) && isfinite(z.re) && isfinite(z.im)) return abs_too_large(vm, "abs", argv[0]);
    return lube->number_value(r);
}

FN(f_isfinite) {
    Cx z;
    LubeValue e;
    if (!arg_of(vm, "isfinite", argv, 0, &z, &e)) return e;
    return lube->boolean(isfinite(z.re) && isfinite(z.im));
}

FN(f_isinf) {
    Cx z;
    LubeValue e;
    if (!arg_of(vm, "isinf", argv, 0, &z, &e)) return e;
    return lube->boolean(isinf(z.re) || isinf(z.im));
}

FN(f_isnan) {
    Cx z;
    LubeValue e;
    if (!arg_of(vm, "isnan", argv, 0, &z, &e)) return e;
    return lube->boolean(isnan(z.re) || isnan(z.im));
}

/* Option `key` of isclose's options object (argument 2), if given: 0, or -1 with the error in *err */
static int option(LubeVM *vm, int argc, const LubeValue *argv, const char *key, double *out, LubeValue *err) {
    if (argc < 3) return 0;
    if (lube->kind(argv[2]) != LUBE_OBJECT) {
        *err = lube->error(vm, "isclose() expects an object of options as argument 3 (such as {rel_tol: ...}), got %s",
                           lube->type_name(argv[2]));
        return -1;
    }
    LubeValue v;
    if (!lube->object_get(vm, argv[2], key, &v)) return 0;
    if (lube->kind(v) != LUBE_NUMBER) {
        *err = lube->error(vm, "isclose(): the option %s is %s, not a Number", key, lube->a_type(v));
        return -1;
    }
    *out = lube->number(v);
    return 0;
}

/* isclose(a, b, {rel_tol: 1e-9, abs_tol: 0}) */
FN(f_isclose) {
    Cx a, b;
    LubeValue e;
    double rel = 1e-9, abs_tol = 0;
    if (!arg_of(vm, "isclose", argv, 0, &a, &e) || !arg_of(vm, "isclose", argv, 1, &b, &e)) return e;
    if (option(vm, argc, argv, "rel_tol", &rel, &e) < 0 || option(vm, argc, argv, "abs_tol", &abs_tol, &e) < 0) return e;
    if (rel < 0 || abs_tol < 0) return lube->error(vm, "isclose(): the tolerances must not be negative");
    if (a.re == b.re && a.im == b.im) return lube->boolean(1);
    if (isinf(a.re) || isinf(a.im) || isinf(b.re) || isinf(b.im)) return lube->boolean(0);
    double diff = c_abs(cx(a.re - b.re, a.im - b.im));
    return lube->boolean(diff <= rel * c_abs(b) || diff <= rel * c_abs(a) || diff <= abs_tol);
}

FN(f_infj) { return new_complex(vm, cx(0.0, INFINITY)); }
FN(f_nanj) { return new_complex(vm, cx(0.0, fabs(NAN))); }

/* ======================================================================
 * Complex: making one, its parts, its operators
 * ==================================================================== */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Whether s starts with word, in any case */
static int starts(const char *s, const char *word) {
    for (; *word; s++, word++)
        if ((*s | 0x20) != *word) return 0;
    return 1;
}

/* A number as Python reads one, at *s: digits with a point and an exponent, or inf, infinity, nan, with a
 * sign or not. 1 with *s past it, or 0 (and *s unmoved). */
static int read_float(const char **s, double *out) {
    const char *p = *s, *start = p;
    if (*p == '+' || *p == '-') p++;
    double sign = *start == '-' ? -1 : 1;
    if (starts(p, "infinity") || starts(p, "inf")) {
        *out = sign * INFINITY;
        *s = p + (starts(p, "infinity") ? 8 : 3);
        return 1;
    }
    if (starts(p, "nan")) {
        *out = sign * fabs(NAN);
        *s = p + 3;
        return 1;
    }
    int digits = 0;
    while (is_digit(*p)) p++, digits++;
    if (*p == '.') {
        p++;
        while (is_digit(*p)) p++, digits++;
    }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') q++;
        if (is_digit(*q)) {
            while (is_digit(*q)) q++;
            p = q;
        }
    }
    *out = strtod(start, NULL);  /* (it stops where this did: no hex, no inf, as checked above) */
    *s = p;
    return 1;
}

/* text as Python's complex() reads it: "1+2j", "-1.5e3J", "j", "(3-4j)", " 2 ", "inf+nanj"; underscores
 * between digits, as in "1_000j". 1 with *out set, or 0. */
static int parse_complex(const char *text, size_t len, Cx *out) {
    char small[128], *buf = len < sizeof small ? small : (char *)malloc(len + 1);
    if (!buf) return 0;  /* LCOV_EXCL_LINE: out of memory */
    size_t n = 0;
    int ok = 1;
    for (size_t i = 0; i < len; i++) {  /* without its underscores, each between two digits */
        if (text[i] != '_') buf[n++] = text[i];
        else if (!(i > 0 && is_digit(text[i - 1]) && i + 1 < len && is_digit(text[i + 1]))) ok = 0;
    }
    buf[n] = '\0';
    const char *s = buf;
    double x = 0, y = 0, z;
    int bracket = 0;
    while (is_space(*s)) s++;
    if (*s == '(') {
        bracket = 1;
        s++;
        while (is_space(*s)) s++;
    }
    if (read_float(&s, &z)) {
        if (*s == '+' || *s == '-') {  /* 1+2j, 1+j */
            x = z;
            if (!read_float(&s, &y)) y = *s++ == '+' ? 1 : -1;
            if (*s != 'j' && *s != 'J') ok = 0;
            else s++;
        } else if (*s == 'j' || *s == 'J') {  /* 2j */
            s++;
            y = z;
        } else {  /* 2 */
            x = z;
        }
    } else {  /* j, +j, -j */
        y = *s == '-' ? -1 : 1;
        if (*s == '+' || *s == '-') s++;
        if (*s != 'j' && *s != 'J') ok = 0;
        else s++;
    }
    while (is_space(*s)) s++;
    if (bracket && *s != ')') ok = 0;
    else if (bracket) s++;
    while (is_space(*s)) s++;
    if ((size_t)(s - buf) != n) ok = 0;  /* (also: a NUL inside) */
    if (buf != small) free(buf);
    *out = cx(x, y);
    return ok;
}

/* Complex(), Complex(re), Complex(re, im), Complex(z), Complex("1+2j") */
FN(f_Complex) {
    Cx z = cx(0.0, 0.0);
    if (argc == 2) {
        if (lube->kind(argv[0]) != LUBE_NUMBER || lube->kind(argv[1]) != LUBE_NUMBER)
            return lube->error(vm, "Complex(re, im) expects two Numbers, got %s and %s", lube->type_name(argv[0]),
                               lube->type_name(argv[1]));
        z = cx(lube->number(argv[0]), lube->number(argv[1]));
    } else if (argc == 1 && lube->kind(argv[0]) == LUBE_STR) {
        size_t len;
        const char *text = lube->str(argv[0], &len);
        if (!parse_complex(text, len, &z))
            return lube->error(vm, "Complex(): \"%.40s%s\" is not a complex number such as \"1+2j\"", text,
                               len > 40 ? "..." : "");
    } else if (argc == 1 && !complex_of(argv[0], &z)) {
        return lube->error(vm, "Complex() expects a Number, a Complex or a str such as \"1+2j\", got %s",
                           lube->type_name(argv[0]));
    }
    return new_complex(vm, z);
}

static const Cx *self(const LubeValue *argv) { return (const Cx *)lube->foreign(argv[0], ComplexType); }

FN(p_real) { return lube->number_value(self(argv)->re); }
FN(p_imag) { return lube->number_value(self(argv)->im); }
FN(m_conjugate) { return new_complex(vm, cx(self(argv)->re, -self(argv)->im)); }

/* "unsupported operand types for '%s': Complex and List", as Lube says it for its own types */
static LubeValue unsupported(LubeVM *vm, const char *op, const LubeValue *argv) {
    return lube->error(vm, "unsupported operand types for '%s': %s and %s", op, lube->type_name(argv[0]),
                       lube->type_name(argv[1]));
}

/* text + z and z + text: the text joined with how z prints, as + does for any value */
static LubeValue join_text(LubeVM *vm, const LubeValue *argv) {
    int text_first = lube->kind(argv[0]) == LUBE_STR;
    size_t len;
    const char *text = lube->str(argv[text_first ? 0 : 1], &len);
    char num[80];
    size_t n = (size_t)describe(self(argv + (text_first ? 1 : 0)), num, sizeof num);
    char small[256], *buf = len + n < sizeof small ? small : (char *)malloc(len + n + 1);
    if (!buf) abort();  /* LCOV_EXCL_LINE: out of memory */
    memcpy(buf + (text_first ? 0 : n), text, len);
    memcpy(buf + (text_first ? len : 0), num, n);
    LubeValue r = lube->new_str(vm, buf, len + n);
    if (buf != small) free(buf);
    return r;
}

/* The two operands of a + b..., each a Complex (*za, 1 in *ca) or a Number (*xa, 0); 0 if one is neither */
static int operands(const LubeValue *argv, Cx *za, Cx *zb, int *ca, int *cb) {
    *ca = lube->foreign(argv[0], ComplexType) != NULL;
    *cb = lube->foreign(argv[1], ComplexType) != NULL;
    return complex_of(argv[0], za) && complex_of(argv[1], zb);
}

/* + - and *: a Number with a Complex is a real number (only the real part is added; both parts multiplied) */
FN(op_add) {
    Cx a, b;
    int ca, cb;
    if (lube->kind(argv[0]) == LUBE_STR || lube->kind(argv[1]) == LUBE_STR) return join_text(vm, argv);
    if (!operands(argv, &a, &b, &ca, &cb)) return unsupported(vm, "+", argv);
    if (ca && cb) return new_complex(vm, cx(a.re + b.re, a.im + b.im));
    return new_complex(vm, ca ? cx(a.re + b.re, a.im) : cx(a.re + b.re, b.im));
}

FN(op_sub) {
    Cx a, b;
    int ca, cb;
    if (!operands(argv, &a, &b, &ca, &cb)) return unsupported(vm, "-", argv);
    if (ca && cb) return new_complex(vm, cx(a.re - b.re, a.im - b.im));
    return new_complex(vm, ca ? cx(a.re - b.re, a.im) : cx(a.re - b.re, -b.im));
}

FN(op_mul) {
    Cx a, b;
    int ca, cb;
    if (!operands(argv, &a, &b, &ca, &cb)) return unsupported(vm, "*", argv);
    if (ca && cb) return new_complex(vm, c_prod(a, b));
    return new_complex(vm, ca ? cx(a.re * b.re, a.im * b.re) : cx(a.re * b.re, a.re * b.im));
}

FN(op_div) {
    Cx a, b, r;
    int ca, cb, err = C_OK;
    if (!operands(argv, &a, &b, &ca, &cb)) return unsupported(vm, "/", argv);
    if (ca && cb) {
        r = c_quot(a, b, &err);
    } else if (ca) {
        if (b.re == 0) err = C_DOMAIN;
        r = cx(a.re / b.re, a.im / b.re);
    } else {
        r = rc_quot(a.re, b, &err);
    }
    if (err != C_OK) return lube->error(vm, "division by zero");
    return new_complex(vm, r);
}

FN(op_pow) {
    Cx a, b;
    int ca, cb, err = C_OK;
    if (!operands(argv, &a, &b, &ca, &cb)) return unsupported(vm, "**", argv);
    Cx r = c_pow(a, b, &err);
    if (err == C_DOMAIN) return lube->error(vm, "0 to a negative or complex power divides by zero");
    if (err == C_RANGE) return lube->error(vm, "Complex **: the result is too large for a Complex");
    return new_complex(vm, r);
}

/* == and !=: equal parts (a Number is a Complex with 0 for its imaginary part); anything else is not equal */
static int equal(const LubeValue *argv) {
    Cx a, b;
    int ca, cb;
    return operands(argv, &a, &b, &ca, &cb) && a.re == b.re && a.im == b.im;
}

FN(op_eq) { return lube->boolean(equal(argv)); }
FN(op_ne) { return lube->boolean(!equal(argv)); }

FN(op_neg) { return new_complex(vm, cx(-self(argv)->re, -self(argv)->im)); }

LUBE_PLUGIN(cmath) {
    lube = api;
    if (api->version < 2) return -1;  /* (operators, properties and constants) */
    static const struct {
        const char *name;
        LubeFn fn;
        int min, max;
    } fns[] = {
        {"acos", f_acos, 1, 1},         {"acosh", f_acosh, 1, 1}, {"asin", f_asin, 1, 1},   {"asinh", f_asinh, 1, 1},
        {"atan", f_atan, 1, 1},         {"atanh", f_atanh, 1, 1}, {"cos", f_cos, 1, 1},     {"cosh", f_cosh, 1, 1},
        {"exp", f_exp, 1, 1},           {"isclose", f_isclose, 2, 3}, {"isfinite", f_isfinite, 1, 1},
        {"isinf", f_isinf, 1, 1},       {"isnan", f_isnan, 1, 1}, {"log", f_log, 1, 2},     {"log10", f_log10, 1, 1},
        {"phase", f_phase, 1, 1},       {"polar", f_polar, 1, 1}, {"rect", f_rect, 2, 2},   {"sin", f_sin, 1, 1},
        {"sinh", f_sinh, 1, 1},         {"sqrt", f_sqrt, 1, 1},   {"tan", f_tan, 1, 1},     {"tanh", f_tanh, 1, 1},
        {"abs", f_abs, 1, 1},           {"infj", f_infj, 0, 0},   {"nanj", f_nanj, 0, 0},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    api->constant(plugin, "pi", PI);
    api->constant(plugin, "e", E);
    api->constant(plugin, "tau", 2 * PI);
    api->constant(plugin, "inf", INFINITY);
    api->constant(plugin, "nan", fabs(NAN));
    ComplexType = api->type(plugin, "Complex", sizeof(Cx), NULL, describe);
    api->constructor(ComplexType, f_Complex, 0, 2);
    api->property(ComplexType, "real", p_real);
    api->property(ComplexType, "imag", p_imag);
    api->method(ComplexType, "conjugate", m_conjugate, 1, 1);
    api->op(ComplexType, "+", op_add);
    api->op(ComplexType, "-", op_sub);
    api->op(ComplexType, "*", op_mul);
    api->op(ComplexType, "/", op_div);
    api->op(ComplexType, "**", op_pow);
    api->op(ComplexType, "==", op_eq);
    api->op(ComplexType, "!=", op_ne);
    api->op(ComplexType, "neg", op_neg);
    return LUBE_PLUGIN_API;
}
