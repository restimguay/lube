/*
 * math.c - the plugin math: Python's math module, all of it, for Lube's
 * Numbers.
 *
 *   import math.*                      (or import math: math.sqrt(2), math.pi)
 *   sqrt(2), cbrt, exp, exp2, expm1, log(x, base?), log2, log10, log1p, pow
 *   sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, asinh, acosh,
 *   atanh, degrees, radians, hypot(x, y, ...), dist([1, 2], [4, 6])
 *   floor, ceil, trunc, modf, frexp, ldexp, fmod, remainder, copysign, fabs,
 *   fma, fmax, fmin, nextafter, ulp
 *   isfinite, isinf, isnan, isnormal, issubnormal, signbit, isclose(a, b, options?)
 *   fsum(xs), prod(xs, options?), sumprod(xs, ys)
 *   factorial, comb, perm, gcd(...), lcm(...), isqrt
 *   gamma, lgamma, erf, erfc
 *   pi, e, tau, inf, nan
 *
 * The results are Python's: the same algorithms where Python has its own
 * (fsum, hypot, dist, sumprod, gamma, lgamma, remainder, nextafter, pow's
 * special cases), the C library's elsewhere. Where Python raises an error, so
 * does this: sqrt(-1) (cmath has complex roots), log(0), exp(1000) (too large
 * for a Number, not inf). With `import math.*` these names replace Lube's own
 * sqrt, floor, log, pow... which give nan or inf for those instead.
 *
 * Lube has no keyword arguments: Python's keyword-only ones come as a last
 * argument, an object: isclose(a, b, {rel_tol: 1e-6, abs_tol: 0}),
 * prod(xs, {start: 2}), nextafter(x, y, {steps: 3}). Python's sequences are
 * Lists, and gcd, lcm and hypot take any number of arguments.
 *
 * Where Lube's Numbers (64-bit floats) make it differ from Python:
 *   - The integer functions (factorial, comb, perm, gcd, isqrt, and prod of
 *     whole numbers) work out the exact integer, as Python does, and give the
 *     Number nearest it: exact up to 2^53, and to 15-17 digits beyond. One too
 *     large for a Number (factorial(171)) is an error. isqrt is exact for n
 *     below 2^106 (roots below 2^53); beyond, it is within a unit or two (a
 *     Number that large is not exact either). lcm is exact while it stays
 *     below 2^53. They take whole Numbers, and sized integers.
 *   - floor, ceil and trunc give Numbers: inf and nan stay as they are (a
 *     Python int cannot hold them, so Python raises), and a sized integer
 *     (int64...) comes back unchanged, as with Lube's own floor.
 *   - math.integer, a module inside math in Python 3.15, is not here: its
 *     functions (comb, factorial, gcd, isqrt, lcm, perm) are math's own.
 */
#include "../lube_plugin.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
/* Every operation rounded on its own, as written: a fused multiply-add the compiler chose would change
 * results from system to system (gcc never fuses under -std=c99). The exact sums below count on it. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "m_sqrt" -> "sqrt" */
#define IS_NUM(v) (lube->kind(v) == LUBE_NUMBER)
#define NEED_NUM(i)                                                         \
    do {                                                                    \
        if (!IS_NUM(argv[i])) return not_a(vm, FNAME, argv, (i), "a Number"); \
    } while (0)
#define NEED_LIST(i)                                                                 \
    do {                                                                             \
        if (lube->kind(argv[i]) != LUBE_LIST) return not_a(vm, FNAME, argv, (i), "a List"); \
    } while (0)
#define X(i) lube->number(argv[i])

static const LubeAPI *lube;

static const double PI = 3.141592653589793238462643383279502884;
static const double LOG_PI = 1.144729885849400174143427351353058711647;

/* ======================================================================
 * Errors
 * ==================================================================== */

/* "sqrt() expects a Number as argument 1, got str" */
static LubeValue not_a(LubeVM *vm, const char *fn, const LubeValue *argv, int i, const char *what) {
    return lube->error(vm, "%s() expects %s as argument %d, got %s", fn, what, i + 1, lube->type_name(argv[i]));
}

/* An argument outside a function's domain: `message` says what it takes, with %s for the number. */
static LubeValue domain_error(LubeVM *vm, const char *message, double x) {
    char num[32];
    lube->format_number(x, num, sizeof num);
    return lube->error(vm, message, num);
}

/* "exp(1000): the result is too large for a Number" */
static LubeValue too_large(LubeVM *vm, const char *fn, const double *args, int n) {
    char text[120];
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        char num[32];
        lube->format_number(args[i], num, sizeof num);
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s", i ? ", " : "", num);
    }
    return lube->error(vm, "%s(%s): the result is too large for a Number", fn, text);
}

/* Argument i, a whole Number, in *out: or 0, with the error in *err. */
static int whole_arg(LubeVM *vm, const char *fn, const LubeValue *argv, int i, double *out, LubeValue *err) {
    if (!IS_NUM(argv[i])) {
        *err = not_a(vm, fn, argv, i, "a whole Number");
        return 0;
    }
    double x = lube->number(argv[i]);
    if (isfinite(x) && x == floor(x)) {
        *out = x;
        return 1;
    }
    char num[32];
    lube->format_number(x, num, sizeof num);
    *err = lube->error(vm, "%s() expects a whole Number as argument %d, got %s", fn, i + 1, num);
    return 0;
}

/* Item i of list (a Number) in *out: or 0, with the error in *err ("fsum(): item 2 is a str, not a Number"). */
static int item_of(LubeVM *vm, const char *fn, LubeValue list, size_t i, double *out, LubeValue *err) {
    LubeValue v = lube->list_get(list, i);
    if (IS_NUM(v)) {
        *out = lube->number(v);
        return 1;
    }
    *err = lube->error(vm, "%s(): item %lu is %s, not a Number", fn, (unsigned long)i, lube->a_type(v));
    return 0;
}

/* Option `key` of the options object, argument i, if there is one: 1 with *out set, 0 if it is not
 * given, or -1 with the error in *err. */
static int option(LubeVM *vm, const char *fn, int argc, const LubeValue *argv, int i, const char *key, double *out,
                  LubeValue *err) {
    if (argc <= i) return 0;
    if (lube->kind(argv[i]) != LUBE_OBJECT) {
        *err = lube->error(vm, "%s() expects an object of options as argument %d (such as {%s: ...}), got %s", fn, i + 1,
                           key, lube->type_name(argv[i]));
        return -1;
    }
    LubeValue v;
    if (!lube->object_get(vm, argv[i], key, &v)) return 0;
    if (!IS_NUM(v)) {
        *err = lube->error(vm, "%s(): the option %s is %s, not a Number", fn, key, lube->a_type(v));
        return -1;
    }
    *out = lube->number(v);
    return 1;
}

/* ======================================================================
 * Exact arithmetic on doubles: sums and products with no rounding lost
 * (Dekker, Knuth, Shewchuk), for fsum, fma, hypot, dist, sumprod and isqrt
 * ==================================================================== */

typedef struct {
    double hi, lo;  /* hi + lo, exactly */
} Pair;

static Pair two_sum(double a, double b) {
    double x = a + b, z = x - a;
    Pair r = {x, (a - (x - z)) + (b - z)};
    return r;
}

static Pair fast_two_sum(double a, double b) {  /* (|a| >= |b|) */
    double x = a + b;
    Pair r = {x, (a - x) + b};
    return r;
}

/* x as hi + lo, each of 26 bits or fewer (Veltkamp), so that products of the parts are exact. */
static Pair split(double x) {
    double s = 1;
    if (fabs(x) > 0x1p995) {  /* (x * 134217729 would overflow: split x / 2^28) */
        x *= 0x1p-28;
        s = 0x1p28;
    }
    double t = x * 134217729.0, hi = t - (t - x);
    Pair r = {hi * s, (x - hi) * s};
    return r;
}

/* a * b: the rounded product, and what rounding it lost (exact unless the product is tiny). */
static Pair two_prod(double a, double b) {
    double x = a * b;
    Pair p = split(a), q = split(b);
    Pair r = {x, p.lo * q.lo - (((x - p.hi * q.hi) - p.lo * q.hi) - p.hi * q.lo)};
    return r;
}

/* Shewchuk's partials, as Python's fsum keeps them: non-overlapping, smallest first, adding up to the
 * exact sum. Adds x to p[0..n): returns how many partials are left, with what remains to add in *x. */
static int partials_add(double *p, int n, double *x) {
    int i = 0;
    double v = *x;
    for (int j = 0; j < n; j++) {
        double y = p[j];
        if (fabs(v) < fabs(y)) {
            double t = v;
            v = y;
            y = t;
        }
        double hi = v + y, lo = y - (hi - v);
        if (lo != 0) p[i++] = lo;
        v = hi;
    }
    *x = v;
    return i;
}

/* The Number nearest the partials' sum (halfway: the even one). */
static double partials_sum(const double *p, int n) {
    double hi = 0, lo = 0;
    if (n > 0) {
        hi = p[--n];
        while (n > 0) {  /* from the top, until the sum is not exact */
            double x = hi, y = p[--n];
            hi = x + y;
            lo = y - (hi - x);
            if (lo != 0) break;
        }
        /* halfway between two Numbers, with more below on the same side: away from the even one */
        if (n > 0 && ((lo < 0 && p[n - 1] < 0) || (lo > 0 && p[n - 1] > 0))) {
            double y = lo * 2, x = hi + y;
            if (y == x - hi) hi = x;
        }
    }
    return hi;
}

/* x * y + z rounded once. Done exactly here where nothing can overflow or underflow (so the same on
 * every system); at the edges, by the C library's fma. */
static double fused(double x, double y, double z) {
    double p = x * y;
    if (!(fabs(p) > 0x1p-900 && fabs(p) < 0x1p1000 && fabs(z) < 0x1p1000)) return fma(x, y, z);
    Pair xy = two_prod(x, y);
    double parts[3] = {0, 0, 0}, v;
    int n = 0;
    double terms[3] = {xy.hi, xy.lo, z};
    for (int i = 0; i < 3; i++) {
        v = terms[i];
        n = partials_add(parts, n, &v);
        if (v != 0) parts[n++] = v;
    }
    return partials_sum(parts, n);
}

/* ======================================================================
 * Whole numbers past 2^53, exactly: for factorial, comb, perm and prod
 * ==================================================================== */

#define BIG_LIMBS 36  /* 1152 bits: 2^1024 (past the largest Number) times a factor */
#define BIG_MAX 1100  /* bits a product may reach (beyond 1025 bits, it is too large for a Number anyway) */

typedef struct {
    int n;                  /* limbs in use (0: the number 0) */
    uint32_t d[BIG_LIMBS];  /* least significant first */
} Big;

static void big_set(Big *b, uint64_t v) {
    b->n = 0;
    while (v) {
        b->d[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static int big_bits(const Big *b) {
    int bits = b->n ? 32 * (b->n - 1) : 0;
    for (uint32_t top = b->n ? b->d[b->n - 1] : 0; top; top >>= 1) bits++;
    return bits;
}

static void big_trim(Big *b) {
    while (b->n > 0 && !b->d[b->n - 1]) b->n--;
}

/* b <<= s (it fits: the callers see to it) */
static void big_shl(Big *b, int s) {
    int q = s / 32, r = s % 32;
    if (r) {
        uint32_t carry = 0;
        for (int i = 0; i < b->n; i++) {
            uint32_t v = b->d[i];
            b->d[i] = v << r | carry;
            carry = v >> (32 - r);
        }
        if (carry) b->d[b->n++] = carry;
    }
    memmove(b->d + q, b->d, sizeof(uint32_t) * (size_t)b->n);
    memset(b->d, 0, sizeof(uint32_t) * (size_t)q);
    b->n += q;
}

/* x, a whole Number above 0 */
static void big_from(Big *b, double x) {
    if (x < 0x1p64) {
        big_set(b, (uint64_t)x);
        return;
    }
    int e;
    double m = frexp(x, &e);  /* x = m * 2^e, m from 0.5 up to 1 */
    big_set(b, (uint64_t)ldexp(m, 53));
    big_shl(b, e - 53);
}

/* a *= b, when big_bits(a) + big_bits(b) <= BIG_MAX */
static void big_mul(Big *a, const Big *b) {
    uint32_t out[2 * BIG_LIMBS] = {0};
    for (int j = 0; j < b->n; j++) {
        uint64_t carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        out[a->n + j] = (uint32_t)carry;
    }
    a->n = a->n + b->n;
    memcpy(a->d, out, sizeof(uint32_t) * (size_t)a->n);
    big_trim(a);
}

/* b /= d, which divides it exactly */
static void big_div(Big *b, uint32_t d) {
    uint64_t r = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        uint64_t cur = r << 32 | b->d[i];
        b->d[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    big_trim(b);
}

/* b += 1, or b -= 1 (b above 0) */
static void big_step(Big *b, int up) {
    for (int i = 0;; i++) {
        if (i == b->n) b->d[b->n++] = 0;
        uint32_t old = b->d[i];
        b->d[i] = up ? old + 1 : old - 1;
        if (up ? b->d[i] != 0 : old != 0) break;
    }
    big_trim(b);
}

/* a -= b (a >= b) */
static void big_sub(Big *a, const Big *b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = t < 0;
        a->d[i] = (uint32_t)(t + (borrow ? (int64_t)1 << 32 : 0));
    }
    big_trim(a);
}

static uint64_t big_limb(const Big *b, int i) { return i < b->n ? b->d[i] : 0; }

/* The Number nearest b (halfway: the even one); inf if it is too large. */
static double big_double(const Big *b) {
    int bits = big_bits(b);
    if (bits <= 53) return (double)(big_limb(b, 0) | big_limb(b, 1) << 32);
    uint64_t top;  /* the 64 bits at the top, and whether any below them is set */
    int sticky = 0;
    if (bits <= 64) {
        top = (big_limb(b, 0) | big_limb(b, 1) << 32) << (64 - bits);
    } else {
        int lo = bits - 64, q = lo / 32, r = lo % 32;
        top = r ? big_limb(b, q) >> r | big_limb(b, q + 1) << (32 - r) | big_limb(b, q + 2) << (64 - r)
                : big_limb(b, q) | big_limb(b, q + 1) << 32;
        for (int i = 0; i < q; i++) sticky |= b->d[i] != 0;
        if (r && (b->d[q] & ((1u << r) - 1))) sticky = 1;
    }
    uint64_t mant = top >> 11, rest = top & 0x7FF;
    if (rest > 0x400 || (rest == 0x400 && (sticky || (mant & 1)))) mant++;
    return ldexp((double)mant, bits - 53);
}

/* perm(n, k) = n! / (n-k)! for whole n >= k >= 0, the Number nearest it: inf if too large. */
static double perm_of(double n, double k) {
    Big acc, f;
    big_set(&acc, 1);
    big_from(&f, n);
    for (double i = 0; i < k; i++) {  /* (each factor is 2 or more but the last: done in BIG_MAX steps) */
        if (big_bits(&acc) + big_bits(&f) > BIG_MAX) return INFINITY;
        big_mul(&acc, &f);
        big_step(&f, 0);
    }
    return big_double(&acc);
}

/* comb(n, k) for whole n >= k >= 0, likewise. */
static double comb_of(double n, double k) {
    if (k > n - k) k = n - k;
    if (k == 0) return 1;
    Big acc, f, kk;
    big_set(&acc, 1);
    big_from(&f, n);  /* f = n - k + 1 */
    big_from(&kk, k);
    big_sub(&f, &kk);
    big_step(&f, 1);
    /* After step i, acc is comb(n - k + i, i): at most comb(n, k), and at least comb(2i, i), past 2^1100
     * before i reaches 600: so a product too large means comb(n, k) is too, and i stays small. */
    for (double i = 1; i <= k; i++) {
        if (big_bits(&acc) + big_bits(&f) > BIG_MAX) return INFINITY;
        big_mul(&acc, &f);
        big_div(&acc, (uint32_t)i);
        big_step(&f, 1);
    }
    return big_double(&acc);
}

/* ======================================================================
 * Gamma: Python's own (Lanczos), the same on every system
 * ==================================================================== */

#define LANCZOS_N 13
static const double lanczos_g = 6.024680040776729583740234375;
static const double lanczos_g_minus_half = 5.524680040776729583740234375;
static const double lanczos_num[LANCZOS_N] = {
    23531376880.410759688572007674451636754734846804940, 42919803642.649098768957899047001988850926355848959,
    35711959237.355668049440185451547166705960488635843, 17921034426.037209699919755754458931112671403265390,
    6039542586.3520280050642916443072979210699388420708, 1439720407.3117216736632230727949123939715485786772,
    248874557.86205415651146038641322942321632125127801, 31426415.585400194380614231628318205362874684987640,
    2876370.6289353724412254090516208496135991145378768, 186056.26539522349504029498971604569928220784236328,
    8071.6720023658162106380029022722506138218516325024, 210.82427775157934587250973392071336271166969580291,
    2.5066282746310002701649081771338373386264310793408};
static const double lanczos_den[LANCZOS_N] = {0.0,      39916800.0, 120543840.0, 150917976.0, 105258076.0,
                                              45995730.0, 13339535.0, 2637558.0,   357423.0,    32670.0,
                                              1925.0,     66.0,       1.0};
#define NGAMMA_INTEGRAL 23
static const double gamma_integral[NGAMMA_INTEGRAL] = {
    1.0,          1.0,           2.0,            6.0,             24.0,             120.0,
    720.0,        5040.0,        40320.0,        362880.0,        3628800.0,        39916800.0,
    479001600.0,  6227020800.0,  87178291200.0,  1307674368000.0, 20922789888000.0, 355687428096000.0,
    6402373705728000.0, 121645100408832000.0, 2432902008176640000.0, 51090942171709440000.0,
    1124000727777607680000.0};

static double lanczos_sum(double x) {  /* (x > 0) */
    double num = 0, den = 0;
    if (x < 5.0) {
        for (int i = LANCZOS_N; --i >= 0;) {
            num = num * x + lanczos_num[i];
            den = den * x + lanczos_den[i];
        }
    } else {  /* (as a function of 1/x: no overflow) */
        for (int i = 0; i < LANCZOS_N; i++) {
            num = num / x + lanczos_num[i];
            den = den / x + lanczos_den[i];
        }
    }
    return num / den;
}

/* sin(pi * x), exact at whole and half numbers */
static double sin_pi(double x) {
    double y = fmod(fabs(x), 2.0), r;
    switch ((int)round(2.0 * y)) {
    case 0: r = sin(PI * y); break;
    case 1: r = cos(PI * (y - 0.5)); break;
    case 2: r = sin(PI * (1.0 - y)); break;  /* (not -sin(pi * (y - 1)): that is -0 at y = 1) */
    case 3: r = -cos(PI * (y - 1.5)); break;
    default: r = sin(PI * (y - 2.0)); break;
    }
    return copysign(1.0, x) * r;
}

enum { GAMMA_OK, GAMMA_DOMAIN, GAMMA_RANGE };  /* how gamma_of and lgamma_of went */

static double gamma_of(double x, int *err) {
    if (!isfinite(x)) {
        if (isnan(x) || x > 0) return x;
        *err = GAMMA_DOMAIN;  /* gamma(-inf) */
        return NAN;
    }
    if (x == 0) {
        *err = GAMMA_DOMAIN;
        return copysign(INFINITY, x);
    }
    if (x == floor(x)) {
        if (x < 0) {
            *err = GAMMA_DOMAIN;
            return NAN;
        }
        if (x <= NGAMMA_INTEGRAL) return gamma_integral[(int)x - 1];
    }
    double absx = fabs(x), r;
    if (absx < 1e-20) {  /* tiny: 1 / x */
        r = 1.0 / x;
        if (isinf(r)) *err = GAMMA_RANGE;
        return r;
    }
    if (absx > 200.0) {  /* overflows, or underflows to 0 */
        if (x < 0) return 0.0 / sin_pi(x);
        *err = GAMMA_RANGE;
        return INFINITY;
    }
    double y = absx + lanczos_g_minus_half, z;
    if (absx > lanczos_g_minus_half) {  /* the error in y, to correct for */
        double q = y - absx;
        z = q - lanczos_g_minus_half;
    } else {
        double q = y - lanczos_g_minus_half;
        z = q - absx;
    }
    z = z * lanczos_g / y;
    if (x < 0) {
        r = -PI / sin_pi(absx) / absx * exp(y) / lanczos_sum(absx);
        r -= z * r;
        if (absx < 140.0) {
            r /= pow(y, absx - 0.5);
        } else {
            double sqrtpow = pow(y, absx / 2.0 - 0.25);
            r /= sqrtpow;
            r /= sqrtpow;
        }
    } else {
        r = lanczos_sum(absx) / exp(y);
        r += z * r;
        if (absx < 140.0) {
            r *= pow(y, absx - 0.5);
        } else {
            double sqrtpow = pow(y, absx / 2.0 - 0.25);
            r *= sqrtpow;
            r *= sqrtpow;
        }
    }
    if (isinf(r)) *err = GAMMA_RANGE;
    return r;
}

static double lgamma_of(double x, int *err) {
    if (!isfinite(x)) return isnan(x) ? x : INFINITY;
    if (x == floor(x) && x <= 2.0) {
        if (x <= 0) {
            *err = GAMMA_DOMAIN;
            return INFINITY;
        }
        return 0.0;  /* lgamma(1) = lgamma(2) = 0 */
    }
    double absx = fabs(x);
    if (absx < 1e-20) return -log(absx);
    double r = log(lanczos_sum(absx)) - lanczos_g;
    r += (absx - 0.5) * (log(absx + lanczos_g - 0.5) - 1);
    if (x < 0) r = LOG_PI - log(fabs(sin_pi(absx))) - log(absx) - r;  /* (reflected) */
    if (isinf(r)) *err = GAMMA_RANGE;
    return r;
}

/* ======================================================================
 * Functions of a Number
 * ==================================================================== */

/* Whether f(x) = r is outside f's domain: nan from a number, or inf from a finite one (a pole). */
static int outside(double x, double r) { return (isnan(r) && !isnan(x)) || (isinf(r) && isfinite(x)); }

/* f(x), checked as Python checks it: nan from a number that is not nan is outside f's domain (`domain`
 * says what f takes), and so is inf from a finite number, unless f `overflows` there. */
static LubeValue unary(LubeVM *vm, const char *fn, const LubeValue *argv, double (*f)(double), int overflows,
                       const char *domain) {
    if (!IS_NUM(argv[0])) return not_a(vm, fn, argv, 0, "a Number");
    double x = lube->number(argv[0]), r = f(x);
    if (isinf(r) && isfinite(x) && overflows) return too_large(vm, fn, &x, 1);
    if (outside(x, r)) return domain_error(vm, domain, x);
    return lube->number_value(r);
}

#define UNARY(name, f, overflows, domain) \
    FN(m_##name) { return unary(vm, #name, argv, f, overflows, domain); }
#define PLAIN(name, expr)                \
    FN(m_##name) {                       \
        NEED_NUM(0);                     \
        double x = X(0);                 \
        return lube->number_value(expr); \
    }

static double log1p_of(double x) { return x == 0 ? x : log1p(x); }  /* (log1p(-0) is -0 everywhere) */

UNARY(sqrt, sqrt, 0, "sqrt() of a negative number (%s): cmath.sqrt gives complex roots")
UNARY(acos, acos, 0, "acos() expects a number from -1 to 1, got %s")
UNARY(asin, asin, 0, "asin() expects a number from -1 to 1, got %s")
UNARY(acosh, acosh, 0, "acosh() expects a number of 1 or more, got %s")
UNARY(atanh, atanh, 0, "atanh() expects a number between -1 and 1 (not either), got %s")
UNARY(cos, cos, 0, "cos() expects a finite number, got %s")
UNARY(sin, sin, 0, "sin() expects a finite number, got %s")
UNARY(tan, tan, 0, "tan() expects a finite number, got %s")
UNARY(log2, log2, 0, "log2() expects a number above 0, got %s")
UNARY(log10, log10, 0, "log10() expects a number above 0, got %s")
UNARY(log1p, log1p_of, 0, "log1p() expects a number above -1, got %s")
UNARY(exp, exp, 1, "exp() is not defined at %s")  /* (never: these overflow, and that is all) */
UNARY(exp2, exp2, 1, "exp2() is not defined at %s")
UNARY(expm1, expm1, 1, "expm1() is not defined at %s")
UNARY(cosh, cosh, 1, "cosh() is not defined at %s")
UNARY(sinh, sinh, 1, "sinh() is not defined at %s")
PLAIN(atan, atan(x))
PLAIN(asinh, asinh(x))
PLAIN(tanh, tanh(x))
PLAIN(cbrt, cbrt(x))
PLAIN(fabs, fabs(x))
PLAIN(erf, erf(x))
PLAIN(erfc, erfc(x))
PLAIN(degrees, x * (180.0 / PI))
PLAIN(radians, x * (PI / 180.0))
FN(m_isfinite) { NEED_NUM(0); return lube->boolean(isfinite(X(0))); }
FN(m_isinf) { NEED_NUM(0); return lube->boolean(isinf(X(0))); }
FN(m_isnan) { NEED_NUM(0); return lube->boolean(isnan(X(0))); }
FN(m_isnormal) { NEED_NUM(0); return lube->boolean(isnormal(X(0))); }
FN(m_issubnormal) { NEED_NUM(0); return lube->boolean(fpclassify(X(0)) == FP_SUBNORMAL); }
FN(m_signbit) { NEED_NUM(0); return lube->boolean(signbit(X(0)) != 0); }

/* log(x), log(x, base) */
FN(m_log) {
    NEED_NUM(0);
    double x = X(0), r = log(x);
    if (outside(x, r)) return domain_error(vm, "log() expects a number above 0, got %s", x);
    if (argc < 2) return lube->number_value(r);
    NEED_NUM(1);
    double b = X(1), den = log(b);
    if (outside(b, den) || den == 0) return domain_error(vm, "log() expects a base above 0 and not 1, got %s", b);
    return lube->number_value(r / den);
}

static LubeValue gamma_like(LubeVM *vm, const char *fn, const LubeValue *argv, double (*f)(double, int *)) {
    if (!IS_NUM(argv[0])) return not_a(vm, fn, argv, 0, "a Number");
    double x = lube->number(argv[0]);
    int err = GAMMA_OK;
    double r = f(x, &err);
    if (err == GAMMA_DOMAIN) {
        char message[100];
        snprintf(message, sizeof message, "%s() expects a number that is not 0 or a negative whole number, got %%s", fn);
        return domain_error(vm, message, x);
    }
    if (err == GAMMA_RANGE) return too_large(vm, fn, &x, 1);
    return lube->number_value(r);
}

FN(m_gamma) { return gamma_like(vm, "gamma", argv, gamma_of); }
FN(m_lgamma) { return gamma_like(vm, "lgamma", argv, lgamma_of); }

/* floor, ceil and trunc: a whole number (or inf, or nan, or a sized integer) as it is */
#define ROUNDING(name, f)                                                         \
    FN(m_##name) {                                                                \
        NEED_NUM(0);                                                              \
        double x = X(0);                                                          \
        return x == floor(x) || isnan(x) ? lube->retain(argv[0]) : lube->number_value(f(x)); \
    }
ROUNDING(floor, floor)
ROUNDING(ceil, ceil)
ROUNDING(trunc, trunc)

/* modf(x): [the fraction, the whole part], both with x's sign */
FN(m_modf) {
    NEED_NUM(0);
    double x = X(0), whole, frac;
    if (isinf(x)) {
        frac = copysign(0.0, x);
        whole = x;
    } else if (isnan(x)) {
        frac = whole = x;
    } else {
        frac = modf(x, &whole);
    }
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, lube->number_value(frac));
    lube->list_push(r, lube->number_value(whole));
    return r;
}

/* frexp(x): [m, e], x = m * 2^e with m from 0.5 up to 1 (or [x, 0] for 0, inf and nan) */
FN(m_frexp) {
    NEED_NUM(0);
    double x = X(0);
    int e = 0;
    if (isfinite(x) && x != 0) x = frexp(x, &e);
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, lube->number_value(x));
    lube->list_push(r, lube->number_value(e));
    return r;
}

/* ulp(x): the value of x's last bit */
FN(m_ulp) {
    NEED_NUM(0);
    double x = fabs(X(0));
    if (!isfinite(x)) return lube->number_value(x);
    double up = nextafter(x, INFINITY);
    return lube->number_value(isinf(up) ? x - nextafter(x, -INFINITY) : up - x);
}

/* ======================================================================
 * Functions of two or more Numbers
 * ==================================================================== */

#define BINARY(name, expr)               \
    FN(m_##name) {                       \
        NEED_NUM(0);                     \
        NEED_NUM(1);                     \
        double x = X(0), y = X(1);       \
        return lube->number_value(expr); \
    }
BINARY(atan2, atan2(x, y))
BINARY(copysign, copysign(x, y))
BINARY(fmax, fmax(x, y))
BINARY(fmin, fmin(x, y))

/* pow(x, y), with Python's answers for inf and nan (which C libraries differ on) */
FN(m_pow) {
    NEED_NUM(0);
    NEED_NUM(1);
    double x = X(0), y = X(1), r;
    if (!isfinite(x) || !isfinite(y)) {
        if (isnan(x)) {
            r = y == 0 ? 1 : x;  /* nan ** 0 is 1 */
        } else if (isnan(y)) {
            r = x == 1 ? 1 : y;  /* 1 ** nan is 1 */
        } else if (isinf(x)) {
            int odd_y = isfinite(y) && fmod(fabs(y), 2.0) == 1.0;
            r = y > 0 ? (odd_y ? x : fabs(x)) : y == 0 ? 1 : odd_y ? copysign(0.0, x) : 0;
        } else {  /* y is inf */
            r = fabs(x) == 1 ? 1 : (y > 0) == (fabs(x) > 1) ? INFINITY : 0;
        }
        return lube->number_value(r);
    }
    r = pow(x, y);
    if (isnan(r)) {
        char num[32], power[32];
        lube->format_number(x, num, sizeof num);
        lube->format_number(y, power, sizeof power);
        return lube->error(vm, "pow(%s, %s): a negative number to a fractional power is complex (cmath's Complex ** gives it)",
                           num, power);
    }
    if (isinf(r)) {
        if (x == 0) return domain_error(vm, "pow(0, %s): 0 to a negative power divides by zero", y);
        double args[2] = {x, y};
        return too_large(vm, "pow", args, 2);
    }
    return lube->number_value(r);
}

/* fmod(x, y) and remainder(x, y): the errors they share */
static LubeValue modulo_error(LubeVM *vm, const char *fn, double x) {
    if (isinf(x)) {
        char message[80];
        snprintf(message, sizeof message, "%s() expects a finite number as argument 1, got %%s", fn);
        return domain_error(vm, message, x);
    }
    return lube->error(vm, "%s(): modulo by zero", fn);
}

/* fmod(x, y): x - n*y for the whole n that leaves it the sign of x (as C's fmod; Lube's % takes y's sign) */
FN(m_fmod) {
    NEED_NUM(0);
    NEED_NUM(1);
    double x = X(0), y = X(1);
    if (isinf(y) && isfinite(x)) return lube->number_value(x);
    double r = fmod(x, y);
    if (isnan(r) && !isnan(x) && !isnan(y)) return modulo_error(vm, "fmod", x);
    return lube->number_value(r);
}

/* remainder(x, y): x - n*y for the whole n nearest x/y (halfway: the even one); always exact */
FN(m_remainder) {
    NEED_NUM(0);
    NEED_NUM(1);
    double x = X(0), y = X(1), r;
    if (isnan(x) || isnan(y)) return lube->number_value(isnan(x) ? x : y);
    if (isinf(x) || y == 0) return modulo_error(vm, "remainder", x);
    if (isinf(y)) return lube->number_value(x);
    double absx = fabs(x), absy = fabs(y), m = fmod(absx, absy), c = absy - m;
    if (m < c) {
        r = m;
    } else if (m > c) {
        r = -c;
    } else {  /* halfway: m if the multiple below is even, else -m */
        r = m - 2.0 * fmod(0.5 * (absx - m), absy);
    }
    return lube->number_value(copysign(1.0, x) * r);
}

/* ldexp(x, i): x * 2^i */
FN(m_ldexp) {
    NEED_NUM(0);
    double x = X(0), i, r;
    LubeValue err;
    if (!whole_arg(vm, "ldexp", argv, 1, &i, &err)) return err;
    if (x == 0 || !isfinite(x)) return lube->number_value(x);
    if (i > INT_MAX) {
        r = INFINITY;
    } else if (i < INT_MIN) {
        r = copysign(0.0, x);
    } else {
        r = ldexp(x, (int)i);
        if (fabs(r) < DBL_MIN) {  /* subnormal: rounded by a multiplication (some C libraries cut it short) */
            int e;
            frexp(x, &e);
            int down = e > DBL_MIN_EXP ? DBL_MIN_EXP - e : 0;  /* (to the lowest normal numbers: no bits lost) */
            r = ldexp(x, down) * ldexp(1.0, (int)i - down);
        }
    }
    if (isinf(r)) {
        double args[2] = {x, i};
        return too_large(vm, "ldexp", args, 2);
    }
    return lube->number_value(r);
}

/* fma(x, y, z): x * y + z, rounded once */
FN(m_fma) {
    NEED_NUM(0);
    NEED_NUM(1);
    NEED_NUM(2);
    double args[3] = {X(0), X(1), X(2)}, r = fused(args[0], args[1], args[2]);
    if (isnan(r) && !isnan(args[0]) && !isnan(args[1]) && !isnan(args[2])) {
        char text[3][32];
        for (int i = 0; i < 3; i++) lube->format_number(args[i], text[i], sizeof text[i]);
        return lube->error(vm, "fma(%s, %s, %s): inf * 0, or inf - inf, has no value", text[0], text[1], text[2]);
    }
    if (isinf(r) && isfinite(args[0]) && isfinite(args[1]) && isfinite(args[2])) return too_large(vm, "fma", args, 3);
    return lube->number_value(r);
}

/* isclose(a, b, {rel_tol: 1e-9, abs_tol: 0}): within either tolerance of each other */
FN(m_isclose) {
    NEED_NUM(0);
    NEED_NUM(1);
    double a = X(0), b = X(1), rel = 1e-9, abs_tol = 0;
    LubeValue err;
    if (option(vm, "isclose", argc, argv, 2, "rel_tol", &rel, &err) < 0) return err;
    if (option(vm, "isclose", argc, argv, 2, "abs_tol", &abs_tol, &err) < 0) return err;
    if (rel < 0 || abs_tol < 0) return lube->error(vm, "isclose(): the tolerances must not be negative");
    if (a == b) return lube->boolean(1);
    if (isinf(a) || isinf(b)) return lube->boolean(0);
    double diff = fabs(b - a);
    return lube->boolean(diff <= fabs(rel * b) || diff <= fabs(rel * a) || diff <= abs_tol);
}

/* nextafter(x, y, {steps: 1}): the Number steps after x toward y */
FN(m_nextafter) {
    NEED_NUM(0);
    NEED_NUM(1);
    double x = X(0), y = X(1), steps = 1;
    LubeValue err;
    int given = option(vm, "nextafter", argc, argv, 2, "steps", &steps, &err);
    if (given < 0) return err;
    if (given && !(steps >= 0 && steps == floor(steps)))
        return domain_error(vm, "nextafter(): steps must be a whole Number, 0 or more, not %s", steps);
    if (steps == 1) return lube->number_value(nextafter(x, y));
    uint64_t n = steps >= 0x1p64 ? UINT64_MAX : (uint64_t)steps;
    if (n == 0 || isnan(x)) return lube->number_value(x);
    if (isnan(y)) return lube->number_value(y);
    uint64_t ux, uy, sign = (uint64_t)1 << 63;  /* (a double's bits count its Numbers, from 0 out) */
    memcpy(&ux, &x, sizeof ux);
    memcpy(&uy, &y, sizeof uy);
    if (ux == uy) return lube->number_value(x);
    uint64_t ax = ux & ~sign, ay = uy & ~sign;
    double r;
    if ((ux ^ uy) & sign) {  /* opposite signs: through 0 */
        if (ax + ay <= n) return lube->number_value(y);
        if (ax < n) {
            uint64_t u = (uy & sign) | (n - ax);
            memcpy(&r, &u, sizeof r);
            return lube->number_value(r);
        }
        ux -= n;
    } else if (ax > ay) {
        if (ax - ay < n) return lube->number_value(y);
        ux -= n;
    } else {
        if (ay - ax < n) return lube->number_value(y);
        ux += n;
    }
    memcpy(&r, &ux, sizeof r);
    return lube->number_value(r);
}

/* ======================================================================
 * Lengths: hypot and dist (Python's vector_norm: nearly always exactly
 * rounded, and the same whatever the order)
 * ==================================================================== */

typedef struct {  /* the coordinates: hypot's arguments, or the differences of dist's points */
    const LubeValue *argv;
    LubeValue p, q;
} Coords;

static double coord(const Coords *c, size_t i) {
    if (c->argv) return fabs(lube->number(c->argv[i]));
    return fabs(lube->number(lube->list_get(c->p, i)) - lube->number(lube->list_get(c->q, i)));
}

static double norm(const Coords *c, size_t n) {
    double max = 0;
    int found_nan = 0;
    for (size_t i = 0; i < n; i++) {
        double x = coord(c, i);
        found_nan |= isnan(x);
        if (x > max) max = x;
    }
    if (isinf(max)) return max;
    if (found_nan) return NAN;
    if (max == 0 || n <= 1) return max;
    int max_e;
    double pre = 1;  /* (subnormal: made normal first, exactly) */
    frexp(max, &max_e);
    if (max_e < -1023) {
        pre = 1 / DBL_MIN;
        frexp(max * pre, &max_e);
    }
    double scale = ldexp(1.0, -max_e), csum = 1, frac1 = 0, frac2 = 0, h, x;
    Pair pr, sm;
    for (size_t i = 0; i < n; i++) {
        x = coord(c, i) * pre * scale;  /* (exact: the largest is from 0.5 up to 1) */
        pr = two_prod(x, x);
        sm = fast_two_sum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    h = sqrt(csum - 1.0 + (frac1 + frac2));
    pr = two_prod(-h, h);
    sm = fast_two_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);  /* (one step of Newton's method) */
    return pre == 1 ? h / scale : DBL_MIN * (h / scale);
}

/* hypot(x, y, ...): the length of the vector */
FN(m_hypot) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    Coords c = {argv, {{0}}, {{0}}};
    return lube->number_value(norm(&c, (size_t)argc));
}

/* dist(p, q): the distance between two points, Lists of their coordinates */
FN(m_dist) {
    NEED_LIST(0);
    NEED_LIST(1);
    size_t n = lube->list_len(argv[0]), m = lube->list_len(argv[1]);
    if (n != m)
        return lube->error(vm, "dist(): the points have different numbers of dimensions (%lu and %lu)", (unsigned long)n,
                           (unsigned long)m);
    LubeValue err;
    double x;
    for (size_t i = 0; i < n; i++)
        if (!item_of(vm, "dist", argv[0], i, &x, &err) || !item_of(vm, "dist", argv[1], i, &x, &err)) return err;
    Coords c = {NULL, argv[0], argv[1]};
    return lube->number_value(norm(&c, n));
}

/* ======================================================================
 * Sums and products of Lists
 * ==================================================================== */

/* fsum(xs): the sum, exactly rounded */
FN(m_fsum) {
    NEED_LIST(0);
    size_t len = lube->list_len(argv[0]);
    double small[32], *p = small, special = 0, inf_sum = 0, x;
    int n = 0, cap = 32;
    LubeValue err;
    for (size_t k = 0; k < len; k++) {
        if (!item_of(vm, "fsum", argv[0], k, &x, &err)) {
            if (p != small) free(p);
            return err;
        }
        double original = x;
        n = partials_add(p, n, &x);
        if (x == 0) continue;
        if (!isfinite(x)) {  /* an inf or nan of the list's, or a sum too large */
            if (isfinite(original)) {
                if (p != small) free(p);
                return lube->error(vm, "fsum(): the sum grows too large for a Number");
            }
            if (isinf(original)) inf_sum += original;
            special += original;
            n = 0;
            continue;
        }
        if (n == cap) {  /* (rare: more than 32 partials) */
            double *bigger = (double *)malloc(sizeof(double) * (size_t)cap * 2);
            if (!bigger) abort();  /* LCOV_EXCL_LINE: out of memory */
            memcpy(bigger, p, sizeof(double) * (size_t)n);
            if (p != small) free(p);
            p = bigger;
            cap *= 2;
        }
        p[n++] = x;
    }
    double sum = partials_sum(p, n);
    if (p != small) free(p);
    if (special != 0) {
        if (isnan(inf_sum)) return lube->error(vm, "fsum(): inf + -inf has no value");
        return lube->number_value(special);
    }
    return lube->number_value(sum);
}

/* The exact product as a Number, with its sign (0 has none, as with Python's ints); inf if too large. */
static double signed_product(const Big *acc, int huge, int negative) {
    double f = huge ? INFINITY : big_double(acc);
    return negative && f != 0 ? -f : f;
}

/* A whole number that prod() multiplies exactly (not -0: that is the float's, as in Python) */
static int exact_whole(double x) { return isfinite(x) && x == floor(x) && !(x == 0 && signbit(x)); }

/* prod(xs, {start: 1}): the product. Whole numbers multiply exactly (as Python's ints), until the first
 * that is not: from there, as Numbers. */
FN(m_prod) {
    NEED_LIST(0);
    double start = 1, f = 0, x;
    LubeValue err;
    if (option(vm, "prod", argc, argv, 1, "start", &start, &err) < 0) return err;
    Big acc, b;
    int exact = exact_whole(start), negative = start < 0, huge = 0;
    big_set(&acc, 0);
    if (exact && start != 0) big_from(&acc, fabs(start));
    if (!exact) f = start;
    size_t len = lube->list_len(argv[0]);
    for (size_t k = 0; k < len; k++) {
        if (!item_of(vm, "prod", argv[0], k, &x, &err)) return err;
        if (exact && exact_whole(x)) {
            negative ^= x < 0;
            if (x == 0) {
                big_set(&acc, 0);
                huge = 0;
            } else if (!huge && acc.n) {
                big_from(&b, fabs(x));
                if (big_bits(&acc) + big_bits(&b) > BIG_MAX) huge = 1;
                else big_mul(&acc, &b);
            }
            continue;
        }
        if (exact) {  /* the first that is not whole: the exact product, as a Number, from here */
            f = signed_product(&acc, huge, negative);
            if (isinf(f)) return lube->error(vm, "prod(): the product is too large for a Number");
            exact = 0;
        }
        f *= x;
    }
    if (exact) f = signed_product(&acc, huge, negative);
    if (exact && isinf(f)) return lube->error(vm, "prod(): the product is too large for a Number");
    return lube->number_value(f);
}

/* The sum of products, kept in triple length (Python's sumprod) */
typedef struct {
    double hi, lo, tiny;
} Triple;

static Triple triple_fma(double x, double y, Triple total) {
    Pair pr = two_prod(x, y), sm = two_sum(total.hi, pr.hi), r1 = two_sum(total.lo, pr.lo), r2 = two_sum(r1.hi, sm.lo);
    Triple t = {sm.hi, r2.hi, total.tiny + r1.lo + r2.lo};
    return t;
}

static double triple_double(Triple t) {
    Pair last = two_sum(t.lo, t.hi);
    return t.tiny + last.lo + last.hi;
}

/* sumprod(xs, ys): xs[0] * ys[0] + xs[1] * ys[1] + ..., in extended precision */
FN(m_sumprod) {
    NEED_LIST(0);
    NEED_LIST(1);
    size_t n = lube->list_len(argv[0]), m = lube->list_len(argv[1]);
    if (n != m)
        return lube->error(vm, "sumprod(): the Lists have different lengths (%lu and %lu)", (unsigned long)n, (unsigned long)m);
    Triple acc = {0, 0, 0};
    double total = 0, x, y;
    int extended = 1, in_use = 0;
    LubeValue err;
    for (size_t i = 0; i < n; i++) {
        if (!item_of(vm, "sumprod", argv[0], i, &x, &err) || !item_of(vm, "sumprod", argv[1], i, &y, &err)) return err;
        if (extended) {
            Triple next = triple_fma(x, y, acc);
            if (isfinite(next.hi)) {
                acc = next;
                in_use = 1;
                continue;
            }
            extended = 0;  /* an inf, a nan or an overflow: plain Numbers from here, as in Python */
            if (in_use) total += triple_double(acc);
            in_use = 0;
        }
        total += x * y;
    }
    if (in_use) total += triple_double(acc);
    return lube->number_value(total);
}

/* ======================================================================
 * Whole numbers
 * ==================================================================== */

/* gcd of two whole numbers 0 or more: exact for every Number (fmod is exact) */
static double gcd_of(double a, double b) {
    if (a < 0x1p64 && b < 0x1p64) {
        uint64_t x = (uint64_t)a, y = (uint64_t)b;
        while (y) {
            uint64_t t = x % y;
            x = y;
            y = t;
        }
        return (double)x;
    }
    while (b != 0) {
        double t = fmod(a, b);
        a = b;
        b = t;
    }
    return a;
}

/* gcd(a, b, ...): the greatest common divisor (gcd() is 0) */
FN(m_gcd) {
    double g = 0, x;
    LubeValue err;
    for (int i = 0; i < argc; i++) {
        if (!whole_arg(vm, "gcd", argv, i, &x, &err)) return err;
        g = gcd_of(g, fabs(x));
    }
    return lube->number_value(g);
}

/* lcm(a, b, ...): the least common multiple (lcm() is 1) */
FN(m_lcm) {
    double l = 1, x;
    LubeValue err;
    for (int i = 0; i < argc; i++) {
        if (!whole_arg(vm, "lcm", argv, i, &x, &err)) return err;
        x = fabs(x);
        if (l == 0 || x == 0) {
            l = 0;
            continue;
        }
        l = l / gcd_of(l, x) * x;  /* (l / gcd is exact) */
        if (isinf(l)) return lube->error(vm, "lcm(): the result is too large for a Number");
    }
    return lube->number_value(l);
}

/* A whole number 0 or more in *out, from argument i (`what` names it in the error) */
static int count_arg(LubeVM *vm, const char *fn, const LubeValue *argv, int i, const char *what, double *out,
                     LubeValue *err) {
    if (!whole_arg(vm, fn, argv, i, out, err)) return 0;
    if (*out >= 0) return 1;
    char num[32];
    lube->format_number(*out, num, sizeof num);
    *err = lube->error(vm, "%s() expects %s, a whole Number 0 or more, got %s", fn, what, num);
    return 0;
}

/* factorial(n): n! */
FN(m_factorial) {
    double n;
    LubeValue err;
    if (!count_arg(vm, "factorial", argv, 0, "n", &n, &err)) return err;
    if (n > 170) return too_large(vm, "factorial", &n, 1);  /* (171! is past 2^1024) */
    return lube->number_value(n <= 1 ? 1 : perm_of(n, n));
}

/* perm(n, k): the ways to pick k of n in order, n! / (n-k)!; perm(n) is n! */
FN(m_perm) {
    double n, k;
    LubeValue err;
    if (!count_arg(vm, "perm", argv, 0, "n", &n, &err)) return err;
    if (argc < 2) k = n;
    else if (!count_arg(vm, "perm", argv, 1, "k", &k, &err)) return err;
    if (k > n) return lube->number_value(0);
    double r = perm_of(n, k), args[2] = {n, k};
    return isinf(r) ? too_large(vm, "perm", args, argc) : lube->number_value(r);
}

/* comb(n, k): the ways to pick k of n, n! / (k! (n-k)!) */
FN(m_comb) {
    double n, k;
    LubeValue err;
    if (!count_arg(vm, "comb", argv, 0, "n", &n, &err)) return err;
    if (!count_arg(vm, "comb", argv, 1, "k", &k, &err)) return err;
    if (k > n) return lube->number_value(0);
    double r = comb_of(n, k), args[2] = {n, k};
    return isinf(r) ? too_large(vm, "comb", args, 2) : lube->number_value(r);
}

/* Whether r*r > n, exactly (r below 2^54, n a whole number) */
static int square_above(double r, double n) {
    Pair sq = two_prod(r, r);
    return (sq.hi - n) + sq.lo > 0;  /* (sq.hi - n is exact when it matters: Sterbenz) */
}

/* isqrt(n): the whole part of n's square root */
FN(m_isqrt) {
    double n;
    LubeValue err;
    if (!count_arg(vm, "isqrt", argv, 0, "n", &n, &err)) return err;
    double r = floor(sqrt(n));
    if (n < 0x1p106) {  /* (r below 2^53: correct it to the exact root) */
        if (square_above(r, n)) r -= 1;
        else if (!square_above(r + 1, n)) r += 1;
    }
    return lube->number_value(r);
}

LUBE_PLUGIN(math) {
    lube = api;
    if (api->version < 2) return -1;  /* (constants) */
    static const struct {
        const char *name;
        LubeFn fn;
        int min, max;
    } fns[] = {
        {"acos", m_acos, 1, 1},           {"acosh", m_acosh, 1, 1},       {"asin", m_asin, 1, 1},
        {"asinh", m_asinh, 1, 1},         {"atan", m_atan, 1, 1},         {"atan2", m_atan2, 2, 2},
        {"atanh", m_atanh, 1, 1},         {"cbrt", m_cbrt, 1, 1},         {"ceil", m_ceil, 1, 1},
        {"comb", m_comb, 2, 2},           {"copysign", m_copysign, 2, 2}, {"cos", m_cos, 1, 1},
        {"cosh", m_cosh, 1, 1},           {"degrees", m_degrees, 1, 1},   {"dist", m_dist, 2, 2},
        {"erf", m_erf, 1, 1},             {"erfc", m_erfc, 1, 1},         {"exp", m_exp, 1, 1},
        {"exp2", m_exp2, 1, 1},           {"expm1", m_expm1, 1, 1},       {"fabs", m_fabs, 1, 1},
        {"factorial", m_factorial, 1, 1}, {"floor", m_floor, 1, 1},       {"fma", m_fma, 3, 3},
        {"fmax", m_fmax, 2, 2},           {"fmin", m_fmin, 2, 2},         {"fmod", m_fmod, 2, 2},
        {"frexp", m_frexp, 1, 1},         {"fsum", m_fsum, 1, 1},         {"gamma", m_gamma, 1, 1},
        {"gcd", m_gcd, 0, -1},            {"hypot", m_hypot, 0, -1},      {"isclose", m_isclose, 2, 3},
        {"isfinite", m_isfinite, 1, 1},   {"isinf", m_isinf, 1, 1},       {"isnan", m_isnan, 1, 1},
        {"isnormal", m_isnormal, 1, 1},   {"isqrt", m_isqrt, 1, 1},       {"issubnormal", m_issubnormal, 1, 1},
        {"lcm", m_lcm, 0, -1},            {"ldexp", m_ldexp, 2, 2},       {"lgamma", m_lgamma, 1, 1},
        {"log", m_log, 1, 2},             {"log10", m_log10, 1, 1},       {"log1p", m_log1p, 1, 1},
        {"log2", m_log2, 1, 1},           {"modf", m_modf, 1, 1},         {"nextafter", m_nextafter, 2, 3},
        {"perm", m_perm, 1, 2},           {"pow", m_pow, 2, 2},           {"prod", m_prod, 1, 2},
        {"radians", m_radians, 1, 1},     {"remainder", m_remainder, 2, 2}, {"signbit", m_signbit, 1, 1},
        {"sin", m_sin, 1, 1},             {"sinh", m_sinh, 1, 1},         {"sqrt", m_sqrt, 1, 1},
        {"sumprod", m_sumprod, 2, 2},     {"tan", m_tan, 1, 1},           {"tanh", m_tanh, 1, 1},
        {"trunc", m_trunc, 1, 1},         {"ulp", m_ulp, 1, 1},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    api->constant(plugin, "pi", PI);
    api->constant(plugin, "e", 2.718281828459045235360287471352662498);
    api->constant(plugin, "tau", 6.283185307179586476925286766559005768);
    api->constant(plugin, "inf", INFINITY);
    api->constant(plugin, "nan", fabs(NAN));
    return LUBE_PLUGIN_API;
}
