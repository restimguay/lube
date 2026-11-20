/*
 * statistics.c - the plugin statistics: Python's statistics module.
 *
 *   import statistics                    (or import statistics.*: its names, bare)
 *   statistics.mean([1, 2, 3, 4])        2.5
 *   mean, fmean(data, weights?), geometric_mean, harmonic_mean(data, weights?),
 *   median, median_low, median_high, median_grouped(data, interval?), mode,
 *   multimode, quantiles(data, {n, method}), pstdev(data, mu?),
 *   pvariance(data, mu?), stdev(data, xbar?), variance(data, xbar?),
 *   covariance(x, y), correlation(x, y, {method}),
 *   linear_regression(x, y, {proportional})   an object: {slope, intercept}
 *   kde(data, h, kernel?)                a Kde: k.pdf(x), k.cdf(x)
 *   kde_random(data, h, kernel?, {seed}) a KdeRandom: r.sample(), r.samples(n)
 *   NormalDist(mu?, sigma?), from_samples(data)
 *                                        d.pdf(x), d.cdf(x), d.inv_cdf(p), d.overlap(e),
 *                                        d.quantiles(n?), d.samples(n, {seed}), d.zscore(x);
 *                                        d.mean, d.median, d.mode, d.stdev, d.variance;
 *                                        d + e, d - e, d * 2, d / 2, -d, d == e
 *
 * Data is a List of Numbers, and Python's keyword arguments come in an object
 * at the end. The answers are Python's: mean, harmonic_mean, the variances
 * and the standard deviations are worked out exactly (as Python's fractions
 * do) and rounded once, and the rest rounds as Python's floats do, step by
 * step (fsum, sumprod, hypot and sum() as Python has them). StatisticsError
 * is a Lube error. Random draws come from the plugin random's generator, so
 * a seed gives the numbers it gives in Python.
 */
#include "../lube_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
#pragma STDC FP_CONTRACT OFF  /* a * b + c rounded twice, as Python does it: never one fma */
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "f_mean" -> "mean" */
#define NEED(cond, i, what)                                                                                    \
    do {                                                                                                       \
        if (!(cond))                                                                                           \
            return lube->error(vm, "%s() expects %s as argument %d, got %s", FNAME, (what), (i) + 1,          \
                               lube->type_name(argv[i]));                                                      \
    } while (0)
#define NEED_NUM(i) NEED(lube->kind(argv[i]) == LUBE_NUMBER, i, "a Number")
#define NUM(i) lube->number(argv[i])
/* argv[i], a List of Numbers, into d (a Data): data_done(&d) lets go of it */
#define DATA(i, d)                                                                                             \
    do {                                                                                                       \
        LubeValue e_ = {{0}};                                                                                  \
        if (!data_of(vm, FNAME, argv, (i), &(d), &e_)) return e_;                                              \
    } while (0)
/* the same for a second List, letting go of the first if this one is wrong */
#define DATA2(i, d, first)                                                                                     \
    do {                                                                                                       \
        LubeValue e_ = {{0}};                                                                                  \
        if (!data_of(vm, FNAME, argv, (i), &(d), &e_)) {                                                       \
            data_done(&(first));                                                                               \
            return e_;                                                                                         \
        }                                                                                                      \
    } while (0)

#define PI 3.141592653589793
#define TAU 6.283185307179586
#define SQRT2 1.4142135623730951     /* sqrt(2.0) */
#define SQRT2PI 2.5066282746310002   /* sqrt(tau) */
#define MAX_WHOLE 9007199254740992.0 /* 2^53 */

static const LubeAPI *lube;
static LubeType *NormalType, *KdeType, *SamplerType;

/* What the plugin random gives others (a copy of random.c's): its generators. */
typedef struct {
    int version;                                 /* 1 */
    size_t size;                                 /* a generator's bytes */
    const char *(*seed)(void *gen, LubeValue a); /* seeds it as Random(a) does: NULL, or why a cannot be a seed */
    double (*random)(void *gen);                 /* random() */
    double (*below)(void *gen, double n);        /* a whole Number from 0 to n - 1, as choice() picks */
    void *(*shared)(int take);                   /* 1: the package's generator, locked; 0: unlocks it */
} RandomGives;

static const RandomGives *rnd;  /* (NULL without the plugin random: then nothing here draws) */
#define GEN_AT 64               /* where a KdeRandom's generator starts in its data */
#define GEN_MAX 4096            /* the most a generator can take */

/* ======================================================================
 * The data: a List of Numbers, as doubles
 * ==================================================================== */

typedef struct {
    double *x;
    size_t n;
    double small[64];
} Data;

static void data_done(Data *d) {
    if (d->x != d->small) free(d->x);
    d->x = d->small;
}

/* argv[i] into d: 0 after raising an error (in *err). */
static int data_of(LubeVM *vm, const char *fn, const LubeValue *argv, int i, Data *d, LubeValue *err) {
    LubeValue v = argv[i];
    d->x = d->small;
    d->n = 0;
    if (lube->kind(v) != LUBE_LIST) {
        *err = lube->error(vm, "%s() expects a List of Numbers as argument %d, got %s", fn, i + 1, lube->type_name(v));
        return 0;
    }
    size_t n = lube->list_len(v);
    if (n > 64 && !(d->x = (double *)malloc(n * sizeof(double)))) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (size_t k = 0; k < n; k++) {
        LubeValue item = lube->list_get(v, k);
        if (lube->kind(item) != LUBE_NUMBER) {
            *err = lube->error(vm, "%s() expects a List of Numbers as argument %d, but item %lu is %s", fn, i + 1,
                               (unsigned long)k, lube->a_type(item));
            data_done(d);
            return 0;
        }
        d->x[k] = lube->number(item);
    }
    d->n = n;
    return 1;
}

/* argv[i] as a whole Number into *out: 0 after raising the error (in *err). */
static int whole_of(LubeVM *vm, const char *fn, const LubeValue *argv, int i, double *out, LubeValue *err) {
    char num[32];
    *out = lube->number(argv[i]);
    if (lube->kind(argv[i]) == LUBE_NUMBER && *out == floor(*out)) return 1;
    lube->format_number(*out, num, sizeof num);
    *err = lube->error(vm, "%s() expects a whole Number as argument %d, got %s", fn, i + 1,
                       lube->kind(argv[i]) == LUBE_NUMBER ? num : lube->type_name(argv[i]));
    return 0;
}

#define WHOLE(i, var)                                                                                          \
    do {                                                                                                       \
        LubeValue e_ = {{0}};                                                                                  \
        if (!whole_of(vm, FNAME, argv, (i), &(var), &e_)) return e_;                                           \
    } while (0)

/* What an options object (argv[i], when it is there) says for key: 1 if it says (*out borrowed). */
static int option(LubeVM *vm, int argc, const LubeValue *argv, int i, const char *key, LubeValue *out) {
    return argc > i && lube->kind(argv[i]) == LUBE_OBJECT && lube->object_get(vm, argv[i], key, out);
}

/* Smallest first; NaN after everything (Python's order with NaN is anyone's guess). */
static int by_value(const void *pa, const void *pb) {
    double a = *(const double *)pa, b = *(const double *)pb;
    if (a < b) return -1;
    if (a > b) return 1;
    return (isnan(a) != 0) - (isnan(b) != 0);
}

static void sort_numbers(double *a, size_t n) { qsort(a, n, sizeof(double), by_value); }

/* Puts what sorting would put at a[k] there, the smaller before it and the larger after (quickselect: the
 * medians need no more than that). */
static void select_kth(double *a, size_t n, size_t k) {
    for (size_t i = 0; i < n; i++)
        if (isnan(a[i])) {
            sort_numbers(a, n);
            return;
        }
    long long lo = 0, hi = (long long)n - 1, kk = (long long)k;
    for (int depth = 0; hi > lo; depth++) {
        if (depth == 64) break;  /* LCOV_EXCL_LINE: (data made to defeat the pivots: sorted below) */
        long long mid = lo + (hi - lo) / 2, i = lo, j = hi;
        double t;
#define SWAP(p, q) (t = a[p], a[p] = a[q], a[q] = t)
        if (a[mid] < a[lo]) SWAP(mid, lo);
        if (a[hi] < a[lo]) SWAP(hi, lo);
        if (a[hi] < a[mid]) SWAP(hi, mid);
        double pivot = a[mid];
        while (i <= j) {  /* (Hoare's: a[lo] and a[hi] keep i and j in) */
            while (a[i] < pivot) i++;
            while (pivot < a[j]) j--;
            if (i <= j) {
                SWAP(i, j);
                i++;
                j--;
            }
        }
#undef SWAP
        if (kk <= j) hi = j;
        else if (kk >= i) lo = i;
        else return;  /* (between them: the pivot's) */
    }
    if (hi > lo) sort_numbers(a + lo, (size_t)(hi - lo + 1));
}

/* ======================================================================
 * Exact sums (Shewchuk's, as Python's fsum has them), and dividing them
 * ==================================================================== */

typedef struct {
    double *p;         /* partials: their sum is exact; none overlap, the smallest first */
    int n, cap;
    int special;       /* an infinity or a NaN was added: then the sum is theirs */
    double specials;   /* those added up, as floats add */
    double infs;       /* the infinities added up */
    int overflow;      /* finite Numbers added up past the largest */
    double small[48];
} Sum;

static void sum_init(Sum *s) {
    s->p = s->small;
    s->n = 0;
    s->cap = 48;
    s->special = s->overflow = 0;
    s->specials = s->infs = 0;
}

static void sum_done(Sum *s) {
    if (s->p != s->small) free(s->p);
}

static void sum_add(Sum *s, double x) {
    if (!isfinite(x)) {
        s->special = 1;
        s->specials += x;
        if (isinf(x)) s->infs += x;
        return;
    }
    int i = 0;
    for (int j = 0; j < s->n; j++) {
        double y = s->p[j];
        if (fabs(x) < fabs(y)) {
            double t = x;
            x = y;
            y = t;
        }
        double hi = x + y, lo = y - (hi - x);
        if (lo != 0.0) s->p[i++] = lo;
        x = hi;
    }
    s->n = i;
    if (x == 0.0) return;
    if (!isfinite(x)) {
        s->overflow = 1;
        s->n = 0;
        return;
    }
    if (s->n == s->cap) {
        double *more = (double *)malloc(sizeof(double) * (size_t)s->cap * 2);
        if (!more) abort();  /* LCOV_EXCL_LINE: out of memory */
        memcpy(more, s->p, sizeof(double) * (size_t)s->n);
        if (s->p != s->small) free(s->p);
        s->p = more;
        s->cap *= 2;
    }
    s->p[s->n++] = x;
}

static void sum_copy(Sum *to, const Sum *from) {
    sum_init(to);
    for (int i = 0; i < from->n; i++) sum_add(to, from->p[i]);
}

/* The partials' sum, rounded once (half to even, across them all). */
static double sum_round(const Sum *s) {
    int n = s->n;
    double hi = 0.0, lo = 0.0;
    if (n > 0) {
        hi = s->p[--n];
        while (n > 0) {
            double x = hi, y = s->p[--n];
            hi = x + y;
            lo = y - (hi - x);
            if (lo != 0.0) break;
        }
        if (n > 0 && ((lo < 0.0 && s->p[n - 1] < 0.0) || (lo > 0.0 && s->p[n - 1] > 0.0))) {
            double y = lo * 2.0, x = hi + y;
            if (y == x - hi) hi = x;
        }
    }
    return hi;
}

/* Python's fsum: NULL with *out, or its error. */
static const char *fsum_of(const Sum *s, double *out) {
    if (s->overflow) return "the sum overflowed along the way";
    if (s->special) {
        *out = s->specials;
        return isnan(s->infs) ? "inf + -inf has no value" : NULL;
    }
    *out = sum_round(s);
    return NULL;
}

/* fsum of n Numbers: NULL with *out, or its error. */
static const char *fsum_array(const double *x, size_t n, double *out) {
    Sum s;
    sum_init(&s);
    for (size_t i = 0; i < n; i++) sum_add(&s, x[i]);
    const char *why = fsum_of(&s, out);
    sum_done(&s);
    return why;
}

/* a * b exactly: *p + *e (Dekker's product; no fma needed) */
static void two_prod(double a, double b, double *p, double *e) {
    if ((fabs(a) > 0x1p996 || fabs(b) > 0x1p996) && isfinite(a) && isfinite(b)) {  /* (past 2^996, splitting overflows) */
        int a_big = fabs(a) >= fabs(b);
        two_prod(a_big ? a * 0x1p-64 : a, a_big ? b : b * 0x1p-64, p, e);
        *p *= 0x1p64;
        *e *= 0x1p64;
        return;
    }
    *p = a * b;
    double c = 134217729.0 * a, ah = c - (c - a), al = a - ah;
    double d = 134217729.0 * b, bh = d - (d - b), bl = b - bh;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* The sign of s - (t[0] + ... ) * (d[0] + ...), exactly. */
static int sign_minus(const Sum *s, const double *t, int nt, const double *d, int nd) {
    Sum r;
    sum_copy(&r, s);
    for (int i = 0; i < nt; i++)
        for (int j = 0; j < nd; j++) {
            double p, e;
            two_prod(t[i], d[j], &p, &e);
            sum_add(&r, -p);
            sum_add(&r, -e);
        }
    double v = sum_round(&r);
    sum_done(&r);
    return (v > 0) - (v < 0);
}

static int odd(double q) {
    int e;
    return (int)((int64_t)ldexp(frexp(fabs(q), &e), 53) & 1);
}

/* q is close: a step at most from where it belongs (far from 0 and from the largest Numbers). */
static int refinable(double q) { return fabs(q) > 1e-290 && fabs(q) < 1e290; }

/* The Number next to q (refinable), upward (up 1) or downward. */
static double next_to(double q, int up) {
    uint64_t bits;
    memcpy(&bits, &q, sizeof bits);
    bits += (q > 0) == up ? 1 : ~(uint64_t)0;
    memcpy(&q, &bits, sizeof q);
    return q;
}

/* s / (d[0] + ...) rounded once, d more than 0: what Python's exact fractions give. */
static double round_div(const Sum *s, const double *d, int nd) {
    double dv = 0;
    for (int i = 0; i < nd; i++) dv += d[i];
    double q = sum_round(s) / dv;
    while (refinable(q)) {
        double up = next_to(q, 1), down = next_to(q, 0);
        double above[2] = {q, (up - q) / 2}, below[2] = {q, -((q - down) / 2)};
        int c = sign_minus(s, above, 2, d, nd);  /* s against the point halfway to up */
        if (c > 0 || (c == 0 && odd(q))) {
            q = up;
            continue;
        }
        c = sign_minus(s, below, 2, d, nd);
        if (c < 0 || (c == 0 && odd(q))) {
            q = down;
            continue;
        }
        break;
    }
    return q;
}

/* sqrt(s / (d[0] + ...)) rounded once: Python's _float_sqrt_of_frac. */
static double sqrt_round(const Sum *s, const double *d, int nd) {
    double q = sqrt(round_div(s, d, nd));
    while (refinable(q)) {
        double up = next_to(q, 1), down = next_to(q, 0), a, b;
        double h = (up - q) / 2, k = (q - down) / 2;
        two_prod(q, q, &a, &b);
        double above[4] = {a, b, 2 * q * h, h * h}, below[4] = {a, b, -2 * q * k, k * k};  /* (q + h)^2, (q - k)^2 */
        int c = sign_minus(s, above, 4, d, nd);
        if (c > 0 || (c == 0 && odd(q))) {
            q = up;
            continue;
        }
        c = sign_minus(s, below, 4, d, nd);
        if (c < 0 || (c == 0 && odd(q))) {
            q = down;
            continue;
        }
        break;
    }
    return q;
}

/* A power of two to multiply the data by, so that its squares neither overflow nor vanish (exactly, but for
 * Numbers far smaller than the largest). */
static double scale_of(const double *x, size_t n) {
    double big = 0;
    for (size_t i = 0; i < n; i++)
        if (isfinite(x[i]) && fabs(x[i]) > big) big = fabs(x[i]);
    if (big == 0 || (big < 1e120 && big > 1e-120)) return 1;
    int e;
    frexp(big, &e);  /* (big is from 2^(e - 1) to 2^e) */
    return ldexp(1.0, e > 1000 ? -1000 : e < -1000 ? 1000 : -e);
}

/* The exact sum of the data into s (of the data times *scale, a power of two, should it overflow). */
static void sum_data(const double *x, size_t n, Sum *s, double *scale) {
    *scale = 1;
    sum_init(s);
    for (size_t i = 0; i < n; i++) sum_add(s, x[i]);
    if (!s->overflow) return;
    *scale = 0x1p-64;
    sum_done(s);
    sum_init(s);
    for (size_t i = 0; i < n; i++) sum_add(s, x[i] * *scale);
}

/* Python's _ss without c: the data's sum of squared deviations from its exact mean, times n, as an exact sum
 * (*num), of the data times scale; *special: the data's infinities and NaNs added up, if it has any (1). */
static int ssd_of(const double *x, size_t n, double scale, Sum *num, double *special) {
    Sum s, e;
    sum_init(&s);
    sum_init(&e);
    sum_init(num);
    for (size_t i = 0; i < n; i++) sum_add(&s, x[i] * scale);
    if (s.special) {
        *special = s.specials;
        sum_done(&s);
        return 1;
    }
    double dn = (double)n, m = sum_round(&s) / dn, p, q;
    for (size_t i = 0; i < n; i++) {  /* e: the sum of (x - m)^2, exactly */
        double xi = x[i] * scale, dh = xi - m, bb = dh - xi, dl = (xi - (dh - bb)) + (-m - bb);
        two_prod(dh, dh, &p, &q);
        sum_add(&e, p);
        sum_add(&e, q);
        if (dl != 0) {
            two_prod(2 * dh, dl, &p, &q);
            sum_add(&e, p);
            sum_add(&e, q);
            two_prod(dl, dl, &p, &q);
            sum_add(&e, p);
            sum_add(&e, q);
        }
    }
    two_prod(dn, m, &p, &q);  /* r = n * (the exact mean - m) */
    sum_add(&s, -p);
    sum_add(&s, -q);
    for (int i = 0; i < e.n; i++) {  /* num = n * e - r^2 */
        two_prod(dn, e.p[i], &p, &q);
        sum_add(num, p);
        sum_add(num, q);
    }
    for (int i = 0; i < s.n; i++)
        for (int j = 0; j < s.n; j++) {
            two_prod(s.p[i], s.p[j], &p, &q);
            sum_add(num, -p);
            sum_add(num, -q);
        }
    sum_done(&s);
    sum_done(&e);
    return 0;
}

/* ======================================================================
 * Python's sumprod, hypot and float pow, step for step
 * ==================================================================== */

typedef struct {
    double hi, lo, tiny;
} Triple;

static void dl_sum(double a, double b, double *x, double *y) {
    *x = a + b;
    double z = *x - a;
    *y = (a - (*x - z)) + (b - z);
}

static Triple tl_fma(double x, double y, Triple t) {
    double ph, pl, sh, sl, r1h, r1l, r2h, r2l;
    two_prod(x, y, &ph, &pl);
    dl_sum(t.hi, ph, &sh, &sl);
    dl_sum(t.lo, pl, &r1h, &r1l);
    dl_sum(r1h, sl, &r2h, &r2l);
    Triple r = {sh, r2h, t.tiny + r1l + r2l};
    return r;
}

/* math.sumprod(x, y) of floats */
static double sumprod(const double *x, const double *y, size_t n) {
    Triple t = {0.0, 0.0, 0.0};
    double total = 0;
    size_t i = 0;
    for (; i < n; i++) {
        Triple next = tl_fma(x[i], y[i], t);
        if (!isfinite(next.hi)) break;  /* (then on as plain floats do) */
        t = next;
    }
    if (i) {
        double hi, lo;
        dl_sum(t.lo, t.hi, &hi, &lo);
        total = t.tiny + lo + hi;
    }
    for (; i < n; i++) total += x[i] * y[i];
    return total;
}

/* math.hypot(a, b) */
static double hypot2(double a, double b) {
    double v[2] = {fabs(a), fabs(b)}, big = v[0] > v[1] ? v[0] : v[1], scale = 1;
    if (isinf(a) || isinf(b)) return INFINITY;
    if (isnan(a) || isnan(b)) return NAN;
    if (big == 0.0) return big;
    int e;
    frexp(big, &e);
    if (e < -1023) return 0x1p-1022 * hypot2(v[0] / 0x1p-1022, v[1] / 0x1p-1022);  /* (subnormal: scaled up first) */
    scale = ldexp(1.0, -e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0, p, q, s;
    for (int i = 0; i < 2; i++) {
        double x = v[i] * scale;
        two_prod(x, x, &p, &q);
        s = csum + p;
        frac2 += (csum - s) + p;
        csum = s;
        frac1 += q;
    }
    double h = sqrt(csum - 1.0 + (frac1 + frac2));
    two_prod(-h, h, &p, &q);
    s = csum + p;
    frac2 += (csum - s) + p;
    csum = s;
    frac1 += q;
    h += (csum - 1.0 + (frac1 + frac2)) / (2.0 * h);
    return h / scale;
}

/* statistics' _sqrtprod: sqrt(x * y), more accurately and without overflowing or vanishing */
static double sqrtprod(double x, double y) {
    double h = sqrt(x * y);
    if (!isfinite(h)) {
        if (isinf(h) && !isinf(x) && !isinf(y)) return sqrtprod(0x1p-512 * x, 0x1p-512 * y) / 0x1p-512;
        return h;
    }
    if (!h) {
        if (x && y) return sqrtprod(0x1p537 * x, 0x1p537 * y) / 0x1p537;
        return h;
    }
    double a[2] = {x, h}, b[2] = {y, -h};
    return h + sumprod(a, b, 2) / (2.0 * h);
}

/* Python's sum() of floats from start (Neumaier's compensated sum), and its parts */
typedef struct {
    double f, c;
    int any;
} PySum;

static void pysum_add(PySum *s, double v) {
    if (!s->any) {
        s->f += v;
        s->any = 1;
        return;
    }
    double t = s->f + v;
    if (fabs(s->f) >= fabs(v)) s->c += (s->f - t) + v;
    else s->c += (v - t) + s->f;
    s->f = t;
}

static double pysum_value(const PySum *s) { return s->c && isfinite(s->c) ? s->f + s->c : s->f; }

/* ======================================================================
 * Averages
 * ==================================================================== */

/* mean(data): exact, rounded once */
FN(f_mean) {
    Data d;
    DATA(0, d);
    if (!d.n) {
        data_done(&d);
        return lube->error(vm, "mean() needs at least one data point");
    }
    double scale, dn = (double)d.n, r;
    Sum s;
    sum_data(d.x, d.n, &s, &scale);
    r = s.special ? s.specials / dn : round_div(&s, &dn, 1) / scale;
    sum_done(&s);
    data_done(&d);
    return lube->number_value(r);
}

/* fmean(data, weights?): with floats, faster */
FN(f_fmean) {
    Data d, w;
    double r, num = 0, den = 1;
    const char *why;
    DATA(0, d);
    if (argc < 2) {
        why = fsum_array(d.x, d.n, &r);
        size_t n = d.n;
        data_done(&d);
        if (why) return lube->error(vm, "fmean(): %s", why);
        if (!n) return lube->error(vm, "fmean() needs at least one data point");
        return lube->number_value(r / (double)n);
    }
    DATA2(1, w, d);
    why = d.n != w.n ? "the data and the weights must be as long as each other" : NULL;
    if (!why) {
        num = sumprod(d.x, w.x, d.n);
        why = fsum_array(w.x, w.n, &den);
        if (!why && !den) why = "the weights must not add up to 0";
    }
    data_done(&d);
    data_done(&w);
    return why ? lube->error(vm, "fmean(): %s", why) : lube->number_value(num / den);
}

/* geometric_mean(data) */
FN(f_geometric_mean) {
    Data d;
    DATA(0, d);
    Sum s;
    int zero = 0;
    double total = 0.0;
    sum_init(&s);
    for (size_t i = 0; i < d.n; i++) {
        double x = d.x[i];
        if (x > 0.0 || isnan(x)) sum_add(&s, log(x));
        else if (x == 0.0) zero = 1;
        else {
            char text[32];
            lube->format_number(x, text, sizeof text);
            sum_done(&s);
            data_done(&d);
            return lube->error(vm, "geometric_mean() takes no negative numbers, got %s", text);
        }
    }
    fsum_of(&s, &total);  /* (logs never overflow, and never are -inf) */
    size_t n = d.n;
    sum_done(&s);
    data_done(&d);
    if (!n) return lube->error(vm, "geometric_mean() needs at least one data point");
    if (isnan(total)) return lube->number_value(NAN);
    if (zero) return lube->number_value(total == INFINITY ? NAN : 0.0);
    return lube->number_value(exp(total / (double)n));
}

/* harmonic_mean(data, weights?): exact, rounded once */
FN(f_harmonic_mean) {
    Data d, w;
    const char *neg = "harmonic_mean() takes no negative numbers";
    DATA(0, d);
    if (!d.n) {
        data_done(&d);
        return lube->error(vm, "harmonic_mean() needs at least one data point");
    }
    if (d.n == 1 && argc < 2) {
        double x = d.x[0];
        data_done(&d);
        return x < 0 ? lube->error(vm, "%s", neg) : lube->number_value(x);
    }
    w.x = w.small;
    w.n = 0;
    if (argc > 1) DATA2(1, w, d);
    const char *why = argc > 1 && w.n != d.n ? "harmonic_mean(): the number of weights does not match the data" : NULL;
    Sum weights, total;
    double r = 0;
    sum_init(&weights);
    sum_init(&total);
    for (size_t i = 0; !why && argc > 1 && i < w.n; i++) {
        if (w.x[i] < 0) why = neg;
        sum_add(&weights, w.x[i]);
    }
    if (argc < 2) sum_add(&weights, (double)d.n);
    int zero = 0;
    for (size_t i = 0; !why && !zero && i < d.n; i++) {
        double wi = argc > 1 ? w.x[i] : 1, x = d.x[i];
        if (x < 0) why = neg;
        else if (wi && x == 0) zero = 1;  /* (a division by zero: Python gives 0) */
        else if (wi) sum_add(&total, wi / x);
    }
    if (!why && !zero) {
        double t = total.special ? total.specials : sum_round(&total);
        if (t <= 0) why = "harmonic_mean(): the weighted sum must be more than 0";
        else if (weights.special || total.special)
            r = (weights.special ? weights.specials : sum_round(&weights)) / t;
        else
            r = round_div(&weights, total.p, total.n);
    }
    sum_done(&weights);
    sum_done(&total);
    data_done(&d);
    data_done(&w);
    return why ? lube->error(vm, "%s", why) : lube->number_value(r);
}

/* median(data), median_low(data), median_high(data) */
static LubeValue median_of(LubeVM *vm, const LubeValue *argv, const char *fn, int which) {
    Data d;
    LubeValue e_ = {{0}};
    if (!data_of(vm, fn, argv, 0, &d, &e_)) return e_;
    size_t n = d.n, k = n / 2;
    if (!n) return lube->error(vm, "%s(): no median for empty data", fn);
    select_kth(d.x, n, k);
    double hi = d.x[k], lo = hi;
    if (n % 2 == 0) {
        lo = d.x[0];
        for (size_t i = 1; i < k; i++)
            if (d.x[i] > lo) lo = d.x[i];
    }
    data_done(&d);
    return lube->number_value(which < 0 ? lo : which > 0 ? hi : n % 2 ? hi : (lo + hi) / 2);
}

FN(f_median) { return median_of(vm, argv, "median", 0); }
FN(f_median_low) { return median_of(vm, argv, "median_low", -1); }
FN(f_median_high) { return median_of(vm, argv, "median_high", 1); }

/* median_grouped(data, interval = 1): the median of data binned around its values, each bin interval wide */
FN(f_median_grouped) {
    Data d;
    double interval = 1;
    if (argc > 1) {
        NEED_NUM(1);
        interval = NUM(1);
    }
    DATA(0, d);
    size_t n = d.n, below = 0, upto = 0;
    if (!n) return lube->error(vm, "median_grouped(): no median for empty data");
    select_kth(d.x, n, n / 2);
    double x = d.x[n / 2];
    for (size_t i = 0; i < n; i++) {
        below += d.x[i] < x;
        upto += d.x[i] <= x;
    }
    data_done(&d);
    double L = x - interval / 2.0;
    return lube->number_value(L + interval * ((double)n / 2 - (double)below) / (double)(upto - below));
}

/* mode and multimode: the items, counted */
typedef struct {
    int kind;
    double num;     /* a Number's, or a Bool's (0 or 1) */
    const char *s;  /* a str's text */
    size_t len, at; /* (at: where it first is) */
} Item;

static int item_cmp(const Item *a, const Item *b) {
    if (a->kind != b->kind) return a->kind - b->kind;
    if (a->kind == LUBE_STR) {
        int c = memcmp(a->s, b->s, a->len < b->len ? a->len : b->len);
        return c ? c : (a->len > b->len) - (a->len < b->len);
    }
    return by_value(&a->num, &b->num);
}

static int by_item(const void *pa, const void *pb) {
    const Item *a = (const Item *)pa, *b = (const Item *)pb;
    int c = item_cmp(a, b);
    return c ? c : (a->at > b->at) - (a->at < b->at);
}

static int by_place(const void *pa, const void *pb) {
    const Item *a = (const Item *)pa, *b = (const Item *)pb;
    return (a->at > b->at) - (a->at < b->at);
}

/* The most common items, in the order they first come: all of them (multi), or the first. */
static LubeValue modes_of(LubeVM *vm, const LubeValue *argv, int multi) {
    const char *fn = multi ? "multimode" : "mode";
    LubeValue data = argv[0];
    if (lube->kind(data) != LUBE_LIST) return lube->error(vm, "%s() expects a List as argument 1, got %s", fn, lube->type_name(data));
    size_t n = lube->list_len(data), groups = 0;
    if (!n && !multi) return lube->error(vm, "mode(): no mode for empty data");
    Item *items = (Item *)malloc(sizeof(Item) * (n + 1));
    if (!items) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (size_t i = 0; i < n; i++) {
        LubeValue v = lube->list_get(data, i);
        Item *it = &items[i];
        it->kind = lube->kind(v);
        it->at = i;
        it->num = it->kind == LUBE_BOOL ? lube->truthy(v) : lube->number(v);
        it->s = lube->str(v, &it->len);
        if (it->kind != LUBE_NUMBER && it->kind != LUBE_STR && it->kind != LUBE_BOOL) {
            free(items);
            return lube->error(vm, "%s() counts Numbers, text and Bools, but item %lu is %s", fn, (unsigned long)i, lube->a_type(v));
        }
    }
    qsort(items, n, sizeof(Item), by_item);
    size_t best = 0;  /* each group's first item (the first in the data) becomes items[group], with its count in len */
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && !item_cmp(&items[i], &items[j])) j++;
        items[groups] = items[i];
        items[groups].len = j - i;
        if (j - i > best) best = j - i;
        groups++;
        i = j;
    }
    qsort(items, groups, sizeof(Item), by_place);
    LubeValue out = multi ? lube->new_list(vm) : lube->nothing();
    for (size_t g = 0; g < groups; g++) {
        if (items[g].len != best) continue;
        LubeValue v = lube->retain(lube->list_get(data, items[g].at));
        if (!multi) {
            out = v;
            break;
        }
        lube->list_push(out, v);
    }
    free(items);
    return out;
}

FN(f_mode) { return modes_of(vm, argv, 0); }
FN(f_multimode) { return modes_of(vm, argv, 1); }

/* quantiles(data, {n = 4, method = "exclusive"}): n - 1 cut points, dividing the data into n groups as likely
 * as each other ("inclusive": the data is the whole population, its least and greatest the ends) */
FN(f_quantiles) {
    double n = 4;
    const char *method = "exclusive";
    LubeValue v;
    if (argc > 1) NEED(lube->kind(argv[1]) == LUBE_OBJECT, 1, "an object of options");
    if (option(vm, argc, argv, 1, "n", &v)) {
        n = lube->number(v);
        if (lube->kind(v) != LUBE_NUMBER || n != floor(n)) return lube->error(vm, "quantiles(): n is a whole Number");
    }
    if (option(vm, argc, argv, 1, "method", &v) && !(method = lube->str(v, NULL)))
        return lube->error(vm, "quantiles(): the method is \"exclusive\" or \"inclusive\"");
    if (n < 1) return lube->error(vm, "quantiles(): n must be at least 1");
    if (n > 2147483647.0) return lube->error(vm, "quantiles(): n is too large");
    Data d;
    DATA(0, d);
    size_t ld = d.n;
    int inclusive = !strcmp(method, "inclusive");
    if (!ld) return lube->error(vm, "quantiles() needs at least one data point");
    if (ld > 1 && !inclusive && strcmp(method, "exclusive")) {
        data_done(&d);
        return lube->error(vm, "quantiles(): unknown method \"%s\" (it is \"exclusive\" or \"inclusive\")", method);
    }
    LubeValue out = lube->new_list(vm);
    if (ld == 1)
        for (double i = 1; i < n; i++) lube->list_push(out, lube->number_value(d.x[0]));
    else {
        sort_numbers(d.x, ld);
        int64_t m = (int64_t)ld + (inclusive ? -1 : 1), nn = (int64_t)n;
        for (int64_t i = 1; i < nn; i++) {
            int64_t j = i * m / nn, delta;
            if (inclusive) {
                delta = i * m - j * nn;
                j++;  /* (data[j] and data[j + 1], as data[j - 1] and data[j] below) */
            } else {
                j = j < 1 ? 1 : j > (int64_t)ld - 1 ? (int64_t)ld - 1 : j;
                delta = i * m - j * nn;
            }
            double a = d.x[j - 1], b = d.x[j];
            lube->list_push(out, lube->number_value((a * (double)(nn - delta) + b * (double)delta) / n));
        }
    }
    data_done(&d);
    return out;
}

/* ======================================================================
 * Spread: the variances and standard deviations
 * ==================================================================== */

/* variance, pvariance, stdev and pstdev: sample (n - 1) or population (n); the square root or not */
static LubeValue spread(LubeVM *vm, int argc, const LubeValue *argv, const char *fn, int sample, int root) {
    Data d;
    LubeValue e_ = {{0}};
    double r = 0, special = 0;
    int is_special;
    if (argc > 1 && lube->kind(argv[1]) != LUBE_NUMBER)
        return lube->error(vm, "%s() expects a Number (the mean) as argument 2, got %s", fn, lube->type_name(argv[1]));
    if (!data_of(vm, fn, argv, 0, &d, &e_)) return e_;
    size_t n = d.n;
    double dn = (double)n, scale = 1, den[2];
    Sum num;
    if (n < (size_t)(sample ? 2 : 1)) {
        data_done(&d);
        return lube->error(vm, "%s() needs at least %s data point%s", fn, sample ? "two" : "one", sample ? "s" : "");
    }
    if (argc > 1) {  /* given the mean c: the exact sum of (x - c)^2, each rounded as floats round */
        double c = NUM(1);
        for (;;) {
            sum_init(&num);
            for (size_t i = 0; i < n; i++) {
                double dv = d.x[i] - c;
                sum_add(&num, dv * dv * (scale * scale));
            }
            if (!num.overflow) break;
            sum_done(&num);
            scale = 0x1p-32;  /* (the squares add up past the largest Number: again, scaled) */
        }
        is_special = num.special;
        special = num.specials;
        den[0] = sample ? dn - 1 : dn;
        den[1] = 0;
    } else {
        scale = scale_of(d.x, n);
        is_special = ssd_of(d.x, n, scale, &num, &special);
        two_prod(dn, sample ? dn - 1 : dn, &den[0], &den[1]);
    }
    data_done(&d);
    if (is_special) {
        sum_done(&num);
        if (root) return lube->error(vm, "%s(): inf or NaN in the data", fn);
        return lube->number_value(special / (sample ? dn - 1 : dn));
    }
    r = root ? sqrt_round(&num, den, 2) / scale : round_div(&num, den, 2) / scale / scale;
    sum_done(&num);
    if (isinf(r)) return lube->error(vm, "%s(): the result is too large for a Number", fn);
    return lube->number_value(r);
}

/* variance(data, xbar?): the sample variance (xbar: the mean, when it is known) */
FN(f_variance) { return spread(vm, argc, argv, "variance", 1, 0); }
/* pvariance(data, mu?): the population variance */
FN(f_pvariance) { return spread(vm, argc, argv, "pvariance", 0, 0); }
FN(f_stdev) { return spread(vm, argc, argv, "stdev", 1, 1); }
FN(f_pstdev) { return spread(vm, argc, argv, "pstdev", 0, 1); }

/* ======================================================================
 * Two inputs: covariance, correlation, linear_regression
 * ==================================================================== */

/* Checks that x and y are as long as each other and at least two long: NULL, or the error. */
static const char *pair_bad(const Data *x, const Data *y) {
    if (x->n != y->n) return "both inputs need the same number of data points";
    if (x->n < 2) return "needs at least two data points";
    return NULL;
}

/* x - its mean (fsum / n), in place: NULL, or fsum's error */
static const char *center(Data *d, double *mean) {
    double total;
    const char *why = fsum_array(d->x, d->n, &total);
    if (why) return why;
    *mean = total / (double)d->n;
    for (size_t i = 0; i < d->n; i++) d->x[i] -= *mean;
    return NULL;
}

#define PAIR(x, y)                                                                                             \
    Data x, y;                                                                                                 \
    const char *why;                                                                                           \
    DATA(0, x);                                                                                                \
    DATA2(1, y, x);                                                                                            \
    if ((why = pair_bad(&x, &y))) {                                                                            \
        data_done(&x);                                                                                         \
        data_done(&y);                                                                                         \
        return lube->error(vm, "%s()%s %s", FNAME, why[0] == 'n' ? "" : ":", why);                             \
    }

/* covariance(x, y): the sample covariance */
FN(f_covariance) {
    PAIR(x, y)
    double xbar, ybar, r = 0;
    if (!(why = center(&x, &xbar)) && !(why = center(&y, &ybar))) r = sumprod(x.x, y.x, x.n) / (double)(x.n - 1);
    data_done(&x);
    data_done(&y);
    return why ? lube->error(vm, "covariance(): %s", why) : lube->number_value(r);
}

/* Python's _rank(data, start): each item's rank, ties averaged, the lowest start */
typedef struct {
    double v;
    size_t at;
} Ranked;

static int by_rank(const void *pa, const void *pb) {
    const Ranked *a = (const Ranked *)pa, *b = (const Ranked *)pb;
    int c = by_value(&a->v, &b->v);
    return c ? c : (a->at > b->at) - (a->at < b->at);
}

static void rank(Data *d, double start) {
    Ranked *r = (Ranked *)malloc(sizeof(Ranked) * (d->n + 1));
    if (!r) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (size_t i = 0; i < d->n; i++) {
        r[i].v = d->x[i];
        r[i].at = i;
    }
    qsort(r, d->n, sizeof(Ranked), by_rank);
    double i = start - 1;
    for (size_t a = 0; a < d->n;) {
        size_t b = a + 1;
        while (b < d->n && r[b].v == r[a].v) b++;
        double size = (double)(b - a), value = i + (size + 1) / 2;
        for (size_t k = a; k < b; k++) d->x[r[k].at] = value;
        i += size;
        a = b;
    }
    free(r);
}

/* correlation(x, y, {method = "linear"}): Pearson's coefficient, or with "ranked", Spearman's */
FN(f_correlation) {
    const char *method = "linear";
    LubeValue v;
    if (argc > 2) NEED(lube->kind(argv[2]) == LUBE_OBJECT, 2, "an object of options");
    if (option(vm, argc, argv, 2, "method", &v) && !(method = lube->str(v, NULL)))
        return lube->error(vm, "correlation(): the method is \"linear\" or \"ranked\"");
    PAIR(x, y)
    double xbar, ybar, r = 0;
    int ranked = !strcmp(method, "ranked");
    if (!ranked && strcmp(method, "linear")) {
        data_done(&x);
        data_done(&y);
        return lube->error(vm, "correlation(): unknown method \"%s\" (it is \"linear\" or \"ranked\")", method);
    }
    if (ranked) {
        double start = ((double)x.n - 1) / -2;  /* (ranks around 0) */
        rank(&x, start);
        rank(&y, start);
    } else if (!(why = center(&x, &xbar))) {
        why = center(&y, &ybar);
    }
    if (!why) {
        double sxy = sumprod(x.x, y.x, x.n), den = sqrtprod(sumprod(x.x, x.x, x.n), sumprod(y.x, y.x, y.n));
        if (den == 0) why = "at least one of the inputs is constant";
        r = sxy / den;
    }
    data_done(&x);
    data_done(&y);
    return why ? lube->error(vm, "correlation(): %s", why) : lube->number_value(r);
}

/* linear_regression(x, y, {proportional = false}): {slope, intercept} of the line that fits best (least
 * squares); proportional: through 0 */
FN(f_linear_regression) {
    LubeValue v;
    if (argc > 2) NEED(lube->kind(argv[2]) == LUBE_OBJECT, 2, "an object of options");
    int proportional = option(vm, argc, argv, 2, "proportional", &v) && lube->truthy(v);
    PAIR(x, y)
    double xbar = 0, ybar = 0, slope = 0, intercept = 0;
    if (!proportional && !(why = center(&x, &xbar))) why = center(&y, &ybar);
    if (!why) {
        double sxy = sumprod(x.x, y.x, x.n) + 0.0, sxx = sumprod(x.x, x.x, x.n);
        if (sxx == 0) why = "x is constant";
        slope = sxy / sxx;
        intercept = proportional ? 0.0 : ybar - slope * xbar;
    }
    data_done(&x);
    data_done(&y);
    if (why) return lube->error(vm, "linear_regression(): %s", why);
    LubeValue out = lube->new_object(vm);
    lube->object_set(vm, out, "slope", lube->number_value(slope));
    lube->object_set(vm, out, "intercept", lube->number_value(intercept));
    return out;
}

/* ======================================================================
 * The normal distribution
 * ==================================================================== */

/* Wichura's algorithm AS241: the inverse of the normal distribution's cdf */
static double inv_cdf(double p, double mu, double sigma) {
    double q = p - 0.5, r, num, den, x;
    if (fabs(q) <= 0.425) {
        r = 0.180625 - q * q;
        num = (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r + 6.7265770927008700853e+4) * r +
                   4.5921953931549871457e+4) * r + 1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
                1.3314166789178437745e+2) * r + 3.3871328727963666080e+0) * q;
        den = (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r + 3.9307895800092710610e+4) * r +
                   2.1213794301586595867e+4) * r + 5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
                4.2313330701600911252e+1) * r + 1.0);
        x = num / den;
        return mu + (x * sigma);
    }
    r = q <= 0.0 ? p : 1.0 - p;
    r = sqrt(-log(r));
    if (r <= 5.0) {
        r = r - 1.6;
        num = (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r + 2.41780725177450611770e-1) * r +
                   1.27045825245236838258e+0) * r + 3.64784832476320460504e+0) * r + 5.76949722146069140550e+0) * r +
                4.63033784615654529590e+0) * r + 1.42343711074968357734e+0);
        den = (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r + 1.51986665636164571966e-2) * r +
                   1.48103976427480074590e-1) * r + 6.89767334985100004550e-1) * r + 1.67638483018380384940e+0) * r +
                2.05319162663775882187e+0) * r + 1.0);
    } else {
        r = r - 5.0;
        num = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r + 1.24266094738807843860e-3) * r +
                   2.65321895265761230930e-2) * r + 2.96560571828504891230e-1) * r + 1.78482653991729133580e+0) * r +
                5.46378491116411436990e+0) * r + 6.65790464350110377720e+0);
        den = (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r + 1.84631831751005468180e-5) * r +
                   7.86869131145613259100e-4) * r + 1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r +
                5.99832206555887937690e-1) * r + 1.0);
    }
    x = num / den;
    if (q < 0.0) x = -x;
    return mu + (x * sigma);
}

typedef struct {
    double mu, sigma;
} Normal;

static int describe_normal(const void *data, char *buf, size_t size) {
    const Normal *d = (const Normal *)data;
    char mu[32], sigma[32];
    lube->format_number(d->mu, mu, sizeof mu);
    lube->format_number(d->sigma, sigma, sizeof sigma);
    return snprintf(buf, size, "NormalDist(mu=%s, sigma=%s)", mu, sigma);
}

static LubeValue new_normal(LubeVM *vm, double mu, double sigma) {
    void *data;
    LubeValue v = lube->new_foreign(vm, NormalType, &data);
    ((Normal *)data)->mu = mu;
    ((Normal *)data)->sigma = sigma;
    return v;
}

static Normal *normal_of(LubeValue v) { return (Normal *)lube->foreign(v, NormalType); }

/* NormalDist(mu = 0, sigma = 1) */
FN(f_NormalDist) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    double mu = argc > 0 ? NUM(0) : 0.0, sigma = argc > 1 ? NUM(1) : 1.0;
    if (sigma < 0.0) return lube->error(vm, "NormalDist(): sigma cannot be negative");
    return new_normal(vm, mu, sigma);
}

/* from_samples(data): the NormalDist of a sample (its mean and sample standard deviation, exact) */
FN(f_from_samples) {
    Data d;
    DATA(0, d);
    size_t n = d.n;
    double scale = scale_of(d.x, n), dn = (double)n, special = 0, den[2], mean, sigma;
    if (n < 2) {
        data_done(&d);
        return lube->error(vm, "from_samples() needs at least two data points");
    }
    Sum num, s;
    double mean_scale;
    sum_data(d.x, n, &s, &mean_scale);
    int is_special = ssd_of(d.x, n, scale, &num, &special);
    data_done(&d);
    two_prod(dn, dn - 1, &den[0], &den[1]);
    if (is_special) {
        mean = special;
        sigma = special / special;
    } else {
        mean = round_div(&s, &dn, 1) / mean_scale;
        sigma = sqrt_round(&num, den, 2) / scale;
    }
    sum_done(&s);
    sum_done(&num);
    return new_normal(vm, mean, sigma);
}

/* d.mean, d.median, d.mode: mu; d.stdev: sigma; d.variance: sigma^2 */
FN(p_mu) { return lube->number_value(normal_of(argv[0])->mu); }
FN(p_stdev) { return lube->number_value(normal_of(argv[0])->sigma); }
FN(p_variance) { return lube->number_value(normal_of(argv[0])->sigma * normal_of(argv[0])->sigma); }

/* A NormalDist's sigma, which must not be 0 for fn: 0 after raising the error in *err. */
static int sigma_ok(LubeVM *vm, const Normal *d, const char *fn, LubeValue *err) {
    if (d->sigma) return 1;
    *err = lube->error(vm, "%s(): not defined when sigma is 0", fn);
    return 0;
}

#define NEED_SIGMA(d)                                                                                          \
    do {                                                                                                       \
        LubeValue e_ = {{0}};                                                                                  \
        if (!sigma_ok(vm, (d), FNAME, &e_)) return e_;                                                         \
    } while (0)

static double cdf(const Normal *d, double x) { return 0.5 * erfc((d->mu - x) / (d->sigma * SQRT2)); }

/* d.pdf(x): the probability density at x */
FN(m_pdf) {
    Normal *d = normal_of(argv[0]);
    NEED_NUM(1);
    NEED_SIGMA(d);
    double z = (NUM(1) - d->mu) / d->sigma;
    return lube->number_value(exp(-0.5 * z * z) / (SQRT2PI * d->sigma));
}

/* d.cdf(x): the probability of a value at most x */
FN(m_cdf) {
    Normal *d = normal_of(argv[0]);
    NEED_NUM(1);
    NEED_SIGMA(d);
    return lube->number_value(cdf(d, NUM(1)));
}

/* d.inv_cdf(p): the value with the probability p of a value at most it */
FN(m_inv_cdf) {
    Normal *d = normal_of(argv[0]);
    NEED_NUM(1);
    double p = NUM(1);
    if (p <= 0.0 || p >= 1.0) return lube->error(vm, "inv_cdf(): p must be more than 0 and less than 1");
    return lube->number_value(inv_cdf(p, d->mu, d->sigma));
}

/* d.quantiles(n = 4): the n - 1 values that cut it into n parts as likely as each other */
FN(m_quantiles) {
    Normal *d = normal_of(argv[0]);
    double n = 4;
    if (argc > 1) WHOLE(1, n);
    LubeValue out = lube->new_list(vm);
    for (double i = 1; i < n; i++) lube->list_push(out, lube->number_value(inv_cdf(i / n, d->mu, d->sigma)));
    return out;
}

/* d.overlap(e): how much the two overlap, from 0 to 1 (the area under both their pdfs) */
FN(m_overlap) {
    Normal *x = normal_of(argv[0]), *y = normal_of(argv[1]), *t;
    NEED(y, 1, "a NormalDist");
    if (y->sigma != x->sigma ? y->sigma < x->sigma : y->mu < x->mu) {  /* (in order: the same either way round) */
        t = x;
        x = y;
        y = t;
    }
    double xv = x->sigma * x->sigma, yv = y->sigma * y->sigma;
    if (!xv || !yv) return lube->error(vm, "overlap(): not defined when sigma is 0");
    double dv = yv - xv, dm = fabs(y->mu - x->mu);
    if (!dv) return lube->number_value(erfc(dm / (2.0 * x->sigma * SQRT2)));
    double a = x->mu * yv - y->mu * xv, b = x->sigma * y->sigma * sqrt(dm * dm + dv * log(yv / xv));
    double x1 = (a + b) / dv, x2 = (a - b) / dv;
    return lube->number_value(1.0 - (fabs(cdf(y, x1) - cdf(x, x1)) + fabs(cdf(y, x2) - cdf(x, x2))));
}

/* d.zscore(x): how many standard deviations x is from the mean */
FN(m_zscore) {
    Normal *d = normal_of(argv[0]);
    NEED_NUM(1);
    NEED_SIGMA(d);
    return lube->number_value((NUM(1) - d->mu) / d->sigma);
}

/* The generator for options' seed (from the plugin random): a new one in buf, seeded with it; or with no seed,
 * the plugin random's own, *shared 1 (locked until generator_done). NULL, or why the seed is wrong. */
static const char *generator(LubeVM *vm, int argc, const LubeValue *argv, int at, double *buf, void **gen, int *shared) {
    LubeValue seed;
    *shared = 0;
    *gen = buf;
    if (!rnd) return "it needs the plugin random, which is not here";  /* LCOV_EXCL_LINE */
    if (option(vm, argc, argv, at, "seed", &seed)) return rnd->seed(buf, seed);
    *shared = 1;
    *gen = rnd->shared(1);
    return NULL;
}

static void generator_done(int shared) {
    if (shared) rnd->shared(0);
}

/* d.samples(n, {seed}): n random values of it (seed: a Random's seed; none: the plugin random's own generator) */
FN(m_samples) {
    Normal *d = normal_of(argv[0]);
    double n, buf[GEN_MAX / sizeof(double)];
    void *gen;
    int shared;
    WHOLE(1, n);
    if (argc > 2) NEED(lube->kind(argv[2]) == LUBE_OBJECT, 2, "an object of options");
    const char *why = generator(vm, argc, argv, 2, buf, &gen, &shared);
    if (why) return lube->error(vm, "samples(): %s", why);
    LubeValue out = lube->new_list(vm);
    for (double i = 0; i < n; i++) lube->list_push(out, lube->number_value(inv_cdf(rnd->random(gen), d->mu, d->sigma)));
    generator_done(shared);
    return out;
}

/* d + e, d - e, d * x, d / x, -d, d == e, as Python's NormalDist has them */
static LubeValue arith(LubeVM *vm, const LubeValue *argv, char op) {
    Normal *a = normal_of(argv[0]), *b = normal_of(argv[1]);
    double x = lube->number(a ? argv[1] : argv[0]);
    int number = lube->kind(a ? argv[1] : argv[0]) == LUBE_NUMBER;
    if (a && b && (op == '+' || op == '-')) return new_normal(vm, op == '+' ? a->mu + b->mu : a->mu - b->mu, hypot2(a->sigma, b->sigma));
    if (!number || (!a && op == '/'))
        return lube->error(vm, "%s %c %s: a NormalDist %s", lube->type_name(argv[0]), op, lube->type_name(argv[1]),
                           op == '+' || op == '-' ? "adds and subtracts Numbers and NormalDists"
                           : op == '*' ? "is multiplied by a Number" : "is divided by a Number");
    Normal *d = a ? a : b;
    switch (op) {
    case '+': return new_normal(vm, d->mu + x, d->sigma);
    case '-': return new_normal(vm, a ? d->mu - x : -(d->mu - x), d->sigma);
    case '*': return new_normal(vm, d->mu * x, d->sigma * fabs(x));
    default:
        if (x == 0) return lube->error(vm, "NormalDist / 0: division by zero");
        return new_normal(vm, d->mu / x, d->sigma / fabs(x));
    }
}

FN(op_add) { return arith(vm, argv, '+'); }
FN(op_sub) { return arith(vm, argv, '-'); }
FN(op_mul) { return arith(vm, argv, '*'); }
FN(op_div) { return arith(vm, argv, '/'); }

FN(op_neg) {
    Normal *d = normal_of(argv[0]);
    return new_normal(vm, -d->mu, d->sigma);
}

static int same(const LubeValue *argv) {
    Normal *a = normal_of(argv[0]), *b = normal_of(argv[1]);
    return a && b && a->mu == b->mu && a->sigma == b->sigma;
}

FN(op_eq) { return lube->boolean(same(argv)); }
FN(op_ne) { return lube->boolean(!same(argv)); }

/* ======================================================================
 * Kernel density estimation: kde and kde_random
 * ==================================================================== */

enum { K_NORMAL, K_LOGISTIC, K_SIGMOID, K_RECTANGULAR, K_TRIANGULAR, K_PARABOLIC, K_QUARTIC, K_TRIWEIGHT, K_COSINE };

static const struct {
    const char *name;
    int kernel;
} KERNELS[] = {
    {"normal", K_NORMAL},       {"gauss", K_NORMAL},         {"logistic", K_LOGISTIC}, {"sigmoid", K_SIGMOID},
    {"rectangular", K_RECTANGULAR}, {"uniform", K_RECTANGULAR}, {"triangular", K_TRIANGULAR},
    {"parabolic", K_PARABOLIC}, {"epanechnikov", K_PARABOLIC}, {"quartic", K_QUARTIC}, {"biweight", K_QUARTIC},
    {"triweight", K_TRIWEIGHT}, {"cosine", K_COSINE},
};

static double k_pdf(int k, double t) {
    switch (k) {
    case K_NORMAL: return exp(-0.5 * t * t) / SQRT2PI;
    case K_LOGISTIC: return 0.5 / (1.0 + cosh(t));
    case K_SIGMOID: return (1 / PI) / cosh(t);
    case K_RECTANGULAR: return 0.5;
    case K_TRIANGULAR: return 1.0 - fabs(t);
    case K_PARABOLIC: return 0.75 * (1.0 - t * t);
    case K_QUARTIC: return 0.9375 * pow(1.0 - t * t, 2.0);
    case K_TRIWEIGHT: return 1.09375 * pow(1.0 - t * t, 3.0);
    default: return (PI / 4) * cos((PI / 2) * t);
    }
}

static double k_cdf(int k, double t) {
    double a[5], b[5];
    switch (k) {
    case K_NORMAL: return 0.5 * erfc(t / -SQRT2);
    case K_LOGISTIC: return 1.0 - 1.0 / (exp(t) + 1.0);
    case K_SIGMOID: return (2 / PI) * atan(exp(t));
    case K_RECTANGULAR: return 0.5 * t + 0.5;
    case K_TRIANGULAR: return t * t * (t < 0.0 ? 0.5 : -0.5) + t + 0.5;
    case K_PARABOLIC:
        a[0] = -0.25, a[1] = 0.75, a[2] = 0.5;
        b[0] = pow(t, 3.0), b[1] = t, b[2] = 1.0;
        return sumprod(a, b, 3);
    case K_QUARTIC:
        a[0] = 0.1875, a[1] = -0.625, a[2] = 0.9375, a[3] = 0.5;
        b[0] = pow(t, 5.0), b[1] = pow(t, 3.0), b[2] = t, b[3] = 1.0;
        return sumprod(a, b, 4);
    case K_TRIWEIGHT:
        a[0] = -0.15625, a[1] = 0.65625, a[2] = -1.09375, a[3] = 1.09375, a[4] = 0.5;
        b[0] = pow(t, 7.0), b[1] = pow(t, 5.0), b[2] = pow(t, 3.0), b[3] = t, b[4] = 1.0;
        return sumprod(a, b, 5);
    default: return 0.5 * sin((PI / 2) * t) + 0.5;
    }
}

/* Where Newton's method starts, for the quartic and triweight kernels' inverse cdf (Python's guesses) */
static double k_estimate(int k, double p) {
    double sign = 1.0, x;
    if (p > 0.5) {
        sign = -1.0;
        p = 1.0 - p;
    }
    if (k == K_QUARTIC) {
        if (p < 0.0106) return (pow(2.0 * p, 0.3838) - 1.0) * sign;
        x = pow(2.0 * p, 0.4258865685331) - 1.0;
        if (p < 0.499) x += 0.026818732 * sin(7.101753784 * p + 2.73230839482953);
        return x * sign;
    }
    x = pow(2.0 * p, 0.3400218741872791) - 1.0;
    if (0.00001 < p && p < 0.499) x -= 0.033 * sin(1.07 * TAU * (p - 0.035));
    return x * sign;
}

static double k_invcdf(int k, double p) {
    double x, diff;
    switch (k) {
    case K_NORMAL: return inv_cdf(p, 0.0, 1.0);
    case K_LOGISTIC: return log(p / (1.0 - p));
    case K_SIGMOID: return log(tan(p * (PI / 2)));
    case K_RECTANGULAR: return 2.0 * p - 1.0;
    case K_TRIANGULAR: return p < 0.5 ? sqrt(2.0 * p) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * p);
    case K_PARABOLIC: return 2.0 * cos((acos(2.0 * p - 1.0) + PI) / 3.0);
    case K_QUARTIC:
    case K_TRIWEIGHT:
        x = k_estimate(k, p);
        while (fabs(diff = k_cdf(k, x) - p) > 1e-12) x -= diff / k_pdf(k, x);
        return x;
    default: return 2.0 * asin(2.0 * p - 1.0) / PI;
    }
}

typedef struct {
    double *data;  /* the points (a Kde with a kernel of limited reach: sorted) */
    size_t n;
    double h;
    int kernel;    /* which of KERNELS */
    int sampler;   /* a KdeRandom (its generator at GEN_AT) */
} Kde;

static void release_kde(void *data) { free(((Kde *)data)->data); }

static int describe_kde(const void *data, char *buf, size_t size) {
    const Kde *k = (const Kde *)data;
    char h[32];
    lube->format_number(k->h, h, sizeof h);
    return snprintf(buf, size, "%s(%s, h=%s, %lu point%s)", k->sampler ? "KdeRandom" : "Kde", KERNELS[k->kernel].name, h,
                    (unsigned long)k->n, k->n == 1 ? "" : "s");
}

/* kde(data, h, kernel = "normal") and kde_random(...): the checks, and the Kde (or KdeRandom) made. */
static LubeValue new_kde(LubeVM *vm, const LubeValue *argv, int argc, const char *fn, LubeType *type, void **out) {
    Data d;
    LubeValue e_ = {{0}};
    const char *name = "normal";
    if (!data_of(vm, fn, argv, 0, &d, &e_)) return e_;
    if (!d.n) {
        data_done(&d);
        return lube->error(vm, "%s(): the data is empty", fn);
    }
    int i = 0, kernels = (int)(sizeof KERNELS / sizeof KERNELS[0]);
    if (lube->kind(argv[1]) != LUBE_NUMBER || NUM(1) <= 0.0) {
        char text[32];
        lube->format_number(NUM(1), text, sizeof text);
        data_done(&d);
        if (lube->kind(argv[1]) != LUBE_NUMBER)
            return lube->error(vm, "%s() expects a Number (the bandwidth h) as argument 2, got %s", fn, lube->type_name(argv[1]));
        return lube->error(vm, "%s(): the bandwidth h must be more than 0, not %s", fn, text);
    }
    if (argc > 2 && !(name = lube->str(argv[2], NULL))) {
        data_done(&d);
        return lube->error(vm, "%s() expects a str (the kernel) as argument 3, got %s", fn, lube->type_name(argv[2]));
    }
    while (i < kernels && strcmp(KERNELS[i].name, name)) i++;
    if (i == kernels) {
        data_done(&d);
        return lube->error(vm, "%s(): unknown kernel \"%s\"", fn, name);
    }
    void *data;
    LubeValue v = lube->new_foreign(vm, type, &data);
    Kde *k = (Kde *)data;
    k->data = (double *)malloc(sizeof(double) * d.n);
    if (!k->data) abort();  /* LCOV_EXCL_LINE: out of memory */
    memcpy(k->data, d.x, sizeof(double) * d.n);
    k->n = d.n;
    k->h = NUM(1);
    k->kernel = i;
    k->sampler = type == SamplerType;
    data_done(&d);
    *out = data;
    return v;
}

static int reaches_all(const Kde *k) { return KERNELS[k->kernel].kernel <= K_SIGMOID; }

/* kde(data, h, kernel = "normal"): a Kde, the estimated density of where data came from (h: the bandwidth) */
FN(f_kde) {
    void *data = NULL;
    LubeValue v = new_kde(vm, argv, argc, "kde", KdeType, &data);
    if (data && !reaches_all((Kde *)data)) sort_numbers(((Kde *)data)->data, ((Kde *)data)->n);
    return v;
}

/* k.pdf(x) (cumulative 0) and k.cdf(x) (1), as Python's functions add them up */
static LubeValue kde_at(LubeVM *vm, const LubeValue *argv, int cumulative) {
    Kde *k = (Kde *)lube->foreign(argv[0], KdeType);
    if (lube->kind(argv[1]) != LUBE_NUMBER)
        return lube->error(vm, "%s() expects a Number as argument 2, got %s", cumulative ? "cdf" : "pdf", lube->type_name(argv[1]));
    double x = NUM(1), h = k->h, n = (double)k->n;
    int kernel = KERNELS[k->kernel].kernel;
    size_t from = 0, to = k->n;
    PySum s = {0, 0, 0};
    if (!reaches_all(k)) {  /* only the points within h of x */
        size_t lo = 0, hi = k->n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (k->data[mid] < x - h) lo = mid + 1;
            else hi = mid;
        }
        from = lo;
        hi = k->n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (x + h < k->data[mid]) hi = mid;
            else lo = mid + 1;
        }
        to = lo;
        if (cumulative) s.f = (double)from;  /* (those below count whole) */
    }
    for (size_t i = from; i < to; i++) {
        double t = (x - k->data[i]) / h;
        pysum_add(&s, cumulative ? k_cdf(kernel, t) : k_pdf(kernel, t));
    }
    return lube->number_value(pysum_value(&s) / (cumulative ? n : n * h));
}

/* k.pdf(x): the estimated density at x */
FN(m_kde_pdf) { return kde_at(vm, argv, 0); }
/* k.cdf(x): the estimated probability of a value at most x */
FN(m_kde_cdf) { return kde_at(vm, argv, 1); }

/* kde_random(data, h, kernel = "normal", {seed}): a KdeRandom, which draws from kde(data, h, kernel) */
FN(f_kde_random) {
    void *data = NULL;
    LubeValue seed = lube->nothing();
    int opts = argc == 3 && lube->kind(argv[2]) == LUBE_OBJECT ? 2 : 3;  /* (kde_random(data, h, {seed})) */
    if (argc > 3) NEED(lube->kind(argv[3]) == LUBE_OBJECT, 3, "an object of options");
    if (!rnd) return lube->error(vm, "kde_random(): it needs the plugin random, which is not here");  /* LCOV_EXCL_LINE */
    LubeValue v = new_kde(vm, argv, opts == 2 ? 2 : argc, "kde_random", SamplerType, &data);
    if (!data) return v;
    option(vm, argc, argv, opts, "seed", &seed);
    const char *why = rnd->seed((char *)data + GEN_AT, seed);
    if (!why) return v;
    lube->release(v);
    return lube->error(vm, "kde_random(): %s", why);
}

static double draw_kde(Kde *k) {
    void *gen = (char *)k + GEN_AT;
    double x = k->data[(size_t)rnd->below(gen, (double)k->n)];
    return x + k->h * k_invcdf(KERNELS[k->kernel].kernel, rnd->random(gen));
}

/* r.sample(): one draw */
FN(m_sample) { return lube->number_value(draw_kde((Kde *)lube->foreign(argv[0], SamplerType))); }

/* r.samples(n): n draws */
FN(k_samples) {
    Kde *k = (Kde *)lube->foreign(argv[0], SamplerType);
    double n;
    WHOLE(1, n);
    LubeValue out = lube->new_list(vm);
    for (double i = 0; i < n; i++) lube->list_push(out, lube->number_value(draw_kde(k)));
    return out;
}

LUBE_PLUGIN(statistics) {
    lube = api;
    if (api->version < 2) return -1;  /* LCOV_EXCL_LINE: (an older Lube) */
    const RandomGives *r = (const RandomGives *)api->require(plugin, "random");
    if (r && r->version >= 1 && r->size <= GEN_MAX) rnd = r;
    static const struct {
        const char *name;
        LubeFn fn;
        int min, max;
    } fns[] = {
        {"mean", f_mean, 1, 1},
        {"fmean", f_fmean, 1, 2},
        {"geometric_mean", f_geometric_mean, 1, 1},
        {"harmonic_mean", f_harmonic_mean, 1, 2},
        {"median", f_median, 1, 1},
        {"median_low", f_median_low, 1, 1},
        {"median_high", f_median_high, 1, 1},
        {"median_grouped", f_median_grouped, 1, 2},
        {"mode", f_mode, 1, 1},
        {"multimode", f_multimode, 1, 1},
        {"quantiles", f_quantiles, 1, 2},
        {"variance", f_variance, 1, 2},
        {"pvariance", f_pvariance, 1, 2},
        {"stdev", f_stdev, 1, 2},
        {"pstdev", f_pstdev, 1, 2},
        {"covariance", f_covariance, 2, 2},
        {"correlation", f_correlation, 2, 3},
        {"linear_regression", f_linear_regression, 2, 3},
        {"from_samples", f_from_samples, 1, 1},
        {"kde", f_kde, 2, 3},
        {"kde_random", f_kde_random, 2, 4},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    NormalType = api->type(plugin, "NormalDist", sizeof(Normal), NULL, describe_normal);
    api->constructor(NormalType, f_NormalDist, 0, 2);
    api->method(NormalType, "pdf", m_pdf, 2, 2);
    api->method(NormalType, "cdf", m_cdf, 2, 2);
    api->method(NormalType, "inv_cdf", m_inv_cdf, 2, 2);
    api->method(NormalType, "quantiles", m_quantiles, 1, 2);
    api->method(NormalType, "overlap", m_overlap, 2, 2);
    api->method(NormalType, "zscore", m_zscore, 2, 2);
    api->method(NormalType, "samples", m_samples, 2, 3);
    api->property(NormalType, "mean", p_mu);
    api->property(NormalType, "median", p_mu);
    api->property(NormalType, "mode", p_mu);
    api->property(NormalType, "stdev", p_stdev);
    api->property(NormalType, "variance", p_variance);
    api->op(NormalType, "+", op_add);
    api->op(NormalType, "-", op_sub);
    api->op(NormalType, "*", op_mul);
    api->op(NormalType, "/", op_div);
    api->op(NormalType, "neg", op_neg);
    api->op(NormalType, "==", op_eq);
    api->op(NormalType, "!=", op_ne);
    KdeType = api->type(plugin, "Kde", sizeof(Kde), release_kde, describe_kde);
    api->method(KdeType, "pdf", m_kde_pdf, 2, 2);
    api->method(KdeType, "cdf", m_kde_cdf, 2, 2);
    SamplerType = api->type(plugin, "KdeRandom", GEN_AT + (rnd ? rnd->size : 0), release_kde, describe_kde);
    api->method(SamplerType, "sample", m_sample, 1, 1);
    api->method(SamplerType, "samples", k_samples, 2, 2);
    return LUBE_PLUGIN_API;
}
