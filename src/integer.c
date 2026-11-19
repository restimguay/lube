/*
 * integer.c - sized integers: int8, int16, int32, int64 and their unsigned
 * uint8 ... uint64 counterparts.
 *
 * Rules, chosen so integers can never silently go wrong:
 *   - Converting a value that does not fit is an error: int8(300).
 *   - Arithmetic is checked: int32(2147483647) + 1 is an "overflow" error,
 *     not a wraparound. Every check happens *before* the C operation, so the
 *     interpreter never performs a signed overflow (undefined behaviour in C).
 *   - Two different integer types do not mix: int32 + int64 is an error;
 *     convert one explicitly. A plain whole number such as `x + 1` is taken
 *     as the integer's own type.
 *   - `/` on integers divides and rounds down; `%` takes the divisor's sign
 *     (the same rule as for plain numbers), so a == (a / b) * b + a % b.
 *   - Comparisons and == work across all number types by exact value.
 */
#include "lube.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *NAMES[] = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64"};
static const int64_t SMIN[] = {INT8_MIN, INT16_MIN, INT32_MIN, INT64_MIN};
static const int64_t SMAX[] = {INT8_MAX, INT16_MAX, INT32_MAX, INT64_MAX};
static const uint64_t UMAX[] = {UINT8_MAX, UINT16_MAX, UINT32_MAX, UINT64_MAX};
/* bounds as doubles: [LO, HI) -- all exactly representable */
static const double LO[] = {-128.0, -32768.0, -2147483648.0, -9223372036854775808.0, 0, 0, 0, 0};
static const double HI[] = {128.0, 32768.0, 2147483648.0, 9223372036854775808.0,
                            256.0, 65536.0, 4294967296.0, 18446744073709551616.0};

const char *int_type_name(int kind) { return NAMES[kind]; }

void int_format(char *buf, size_t size, Value v) {
    if (IK_SIGNED(v.ikind)) snprintf(buf, size, "%lld", (long long)v.as.i);
    else snprintf(buf, size, "%llu", (unsigned long long)v.as.u);
}

static void range_text(char *buf, size_t size, int k) {
    if (IK_SIGNED(k)) snprintf(buf, size, "%lld to %lld", (long long)SMIN[k], (long long)SMAX[k]);
    else snprintf(buf, size, "0 to %llu", (unsigned long long)UMAX[k - IK_U8]);
}

/* A plain number for any numeric value (large int64/uint64 may round). */
double numeric_value(Value v) {
    if (IS_NUM(v)) return v.as.number;
    return IK_SIGNED(v.ikind) ? (double)v.as.i : (double)v.as.u;
}

/* Does the integer have an exactly equal double? (Used so that equal
 * numbers hash alike: 5 and int32(5) are the same map key.) */
int int_exact_double(Value v, double *out) {
    if (IK_SIGNED(v.ikind)) {
        double d = (double)v.as.i;
        if (d >= 9223372036854775808.0 || (int64_t)d != v.as.i) return 0;
        *out = d;
    } else {
        double d = (double)v.as.u;
        if (d >= 18446744073709551616.0 || (uint64_t)d != v.as.u) return 0;
        *out = d;
    }
    return 1;
}

/* Exact comparison of an integer with a double: -1, 0, 1, or 2 for NaN. */
static int compare_int_double(Value iv, double d) {
    if (d != d) return 2;
    double f = floor(d);
    if (IK_SIGNED(iv.ikind)) {
        if (d >= 9223372036854775808.0) return -1;
        if (d < -9223372036854775808.0) return 1;
        int64_t fi = (int64_t)f;  /* in range: checked above */
        if (iv.as.i != fi) return iv.as.i < fi ? -1 : 1;
    } else {
        if (d < 0) return 1;
        if (d >= 18446744073709551616.0) return -1;
        uint64_t fu = (uint64_t)f;
        if (iv.as.u != fu) return iv.as.u < fu ? -1 : 1;
    }
    return d > f ? -1 : 0;  /* equal whole parts: a fraction makes d larger */
}

/* Compares two numeric values exactly: -1, 0, 1, or 2 when unordered (NaN). */
int numeric_compare(Value a, Value b) {
    if (IS_NUM(a) && IS_NUM(b)) {
        double x = a.as.number, y = b.as.number;
        if (x != x || y != y) return 2;
        return x < y ? -1 : x > y;
    }
    if (IS_INT(a) && IS_NUM(b)) return compare_int_double(a, b.as.number);
    if (IS_NUM(a) && IS_INT(b)) {
        int c = compare_int_double(b, a.as.number);
        return c == 2 ? 2 : -c;
    }
    int sa = IK_SIGNED(a.ikind), sb = IK_SIGNED(b.ikind);
    if (sa && sb) return a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
    if (!sa && !sb) return a.as.u < b.as.u ? -1 : a.as.u > b.as.u;
    if (sa) {
        if (a.as.i < 0) return -1;
        uint64_t x = (uint64_t)a.as.i;
        return x < b.as.u ? -1 : x > b.as.u;
    }
    if (b.as.i < 0) return 1;
    uint64_t y = (uint64_t)b.as.i;
    return a.as.u < y ? -1 : a.as.u > y;
}

static Value doesnt_fit(Interp *I, const char *what, int k) {
    char range[96];
    range_text(range, sizeof range, k);
    return lube_error(I, "%s does not fit in %s (%s)", what, NAMES[k], range);
}

/* A double to integer kind k. With truncate, the fraction is dropped;
 * without, a fraction is an error. Never converts an out-of-range double. */
static Value from_double(Interp *I, double d, int k, int truncate) {
    char text[64];
    if (d != d || d == HUGE_VAL || d == -HUGE_VAL) {
        format_number(text, sizeof text, d);
        return lube_error(I, "%s cannot be converted to %s", text, NAMES[k]);
    }
    if (truncate) {
        d = d < 0 ? ceil(d) : floor(d);
    } else if (d != floor(d)) {
        format_number(text, sizeof text, d);
        return lube_error(I, "%s is not a whole number, so it cannot be used as an %s (use num(x) for decimal math)",
                          text, NAMES[k]);
    }
    if (d < LO[k] || d >= HI[k]) {
        format_number(text, sizeof text, d);
        if (fabs(d) >= 9007199254740992.0) {  /* 2^53: plain numbers stop being exact */
            char range[96];
            range_text(range, sizeof range, k);
            return lube_error(I, "%s does not fit in %s (%s); note that plain numbers are exact only up to "
                                 "2^53, so write big values as a string, e.g. %s(\"9223372036854775807\")",
                              text, NAMES[k], range, NAMES[k]);
        }
        return doesnt_fit(I, text, k);
    }
    if (IK_SIGNED(k)) return SINT_VAL(k, (int64_t)d);
    return UINT_VAL(k, (uint64_t)d);
}

/* Like int_from_whole, but only reports success (1) or failure (0). */
int int_try_whole(double d, int kind, Value *out) {
    if (d != d || d != floor(d) || d < LO[kind] || d >= HI[kind]) return 0;
    *out = IK_SIGNED(kind) ? SINT_VAL(kind, (int64_t)d) : UINT_VAL(kind, (uint64_t)d);
    return 1;
}

/* A whole plain number as integer kind k (a fraction or out of range is an
 * error): how 5 becomes an int32 in int32 lists and arithmetic. */
Value int_from_whole(Interp *I, double d, int kind) { return from_double(I, d, kind, 0); }

/* Converts an integer to kind k, checking the range. */
static Value from_int(Interp *I, Value v, int k) {
    char text[64];
    int ok;
    if (IK_SIGNED(v.ikind)) {
        int64_t x = v.as.i;
        ok = IK_SIGNED(k) ? x >= SMIN[k] && x <= SMAX[k] : x >= 0 && (uint64_t)x <= UMAX[k - IK_U8];
        if (ok) return IK_SIGNED(k) ? SINT_VAL(k, x) : UINT_VAL(k, (uint64_t)x);
    } else {
        uint64_t x = v.as.u;
        ok = IK_SIGNED(k) ? x <= (uint64_t)SMAX[k] : x <= UMAX[k - IK_U8];
        if (ok) return IK_SIGNED(k) ? SINT_VAL(k, (int64_t)x) : UINT_VAL(k, x);
    }
    int_format(text, sizeof text, v);
    return doesnt_fit(I, text, k);
}

/* Parses "42", "-7", "0xff", "1_000" (surrounding spaces allowed). */
static Value from_string(Interp *I, ObjString *s, int k) {
    const char *p = s->chars, *end = s->chars + s->length;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    int negative = 0;
    if (p < end && (*p == '-' || *p == '+')) negative = *p++ == '-';
    unsigned base = 10;
    if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    uint64_t mag = 0;
    int digits = 0, too_big = 0;
    for (; p < end; p++) {
        if (*p == '_' && digits > 0) continue;
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p - '0');
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = (unsigned)(*p - 'a' + 10);
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = (unsigned)(*p - 'A' + 10);
        else return lube_error(I, "'%s' is not a whole number", s->chars);
        if (mag > (UINT64_MAX - d) / base) too_big = 1;
        else mag = mag * base + d;
        digits++;
    }
    if (digits == 0) return lube_error(I, "'%s' is not a whole number", s->chars);
    if (too_big) return doesnt_fit(I, s->chars, k);
    if (!negative) return from_int(I, UINT_VAL(IK_U64, mag), k);
    if (mag == 0) return from_int(I, SINT_VAL(IK_I64, 0), k);
    if (mag > (uint64_t)INT64_MAX + 1) return doesnt_fit(I, s->chars, k);
    int64_t x = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
    return from_int(I, SINT_VAL(IK_I64, x), k);
}

/* What int32(v) and friends do. */
Value int_convert(Interp *I, Value v, int k) {
    if (IS_INT(v)) return from_int(I, v, k);
    if (IS_NUM(v)) return from_double(I, v.as.number, k, 1);
    if (IS_BOOL(v)) return IK_SIGNED(k) ? SINT_VAL(k, v.as.boolean) : UINT_VAL(k, (uint64_t)v.as.boolean);
    if (IS_TEXT(v)) return from_string(I, text_of(v), k);
    return lube_error(I, "cannot convert %s to %s", a_type(v), NAMES[k]);
}

/* The operators that can overflow (% never does). */
static const char *op_text(int op) {
    switch (op) {
    case T_PLUS: return "+";
    case T_MINUS: return "-";
    case T_STAR: return "*";
    case T_SLASH: return "/";
    default: return "**";  /* T_POW */
    }
}

static Value overflow(Interp *I, int k, Value a, int op, Value b) {
    char x[64], y[64], range[96];
    int_format(x, sizeof x, a);
    int_format(y, sizeof y, b);
    range_text(range, sizeof range, k);
    return lube_error(I, "%s overflow: %s %s %s is outside %s", NAMES[k], x, op_text(op), y, range);
}

/* --- signed, computed in int64 with every overflow checked first --- */

static int smul(int64_t a, int64_t b, int64_t *r) {  /* 0 = overflow */
    if (a == 0 || b == 0) { *r = 0; return 1; }
    if (a > 0) {
        if (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a) return 0;
    } else {
        if (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b) return 0;
    }
    *r = a * b;
    return 1;
}

static Value signed_op(Interp *I, int k, int op, Value va, Value vb) {
    int64_t a = va.as.i, b = vb.as.i, r = 0;
    int ok = 1;
    switch (op) {
    case T_PLUS:
        ok = !((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b));
        if (ok) r = a + b;
        break;
    case T_MINUS:
        ok = !((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b));
        if (ok) r = a - b;
        break;
    case T_STAR:
        ok = smul(a, b, &r);
        break;
    case T_SLASH:
        if (b == 0) return lube_error(I, "division by zero");
        if (a == INT64_MIN && b == -1) { ok = 0; break; }
        r = a / b;
        if (a % b != 0 && ((a < 0) != (b < 0))) r--;  /* round down */
        break;
    case T_PERCENT:
        if (b == 0) return lube_error(I, "modulo by zero");
        r = b == -1 ? 0 : a % b;  /* INT64_MIN % -1 is undefined in C */
        if (r != 0 && ((r < 0) != (b < 0))) r += b;
        break;
    default: {  /* T_POW: int_arith is only given arithmetic operators */
        if (b < 0) return lube_error(I, "an integer power needs a non-negative exponent (use num(x) for fractions)");
        /* square-and-multiply; if squaring the base no longer fits while
         * exponent bits remain, the final result cannot fit either */
        int64_t base = a, e = b;
        r = 1;
        while (e > 0 && ok) {
            if (e & 1) ok = smul(r, base, &r) && r >= SMIN[k] && r <= SMAX[k];
            e >>= 1;
            if (e > 0 && ok) ok = smul(base, base, &base) && base >= SMIN[k] && base <= SMAX[k];
        }
        break;
    }
    }
    if (!ok || r < SMIN[k] || r > SMAX[k]) return overflow(I, k, va, op, vb);
    return SINT_VAL(k, r);
}

/* --- unsigned, computed in uint64 with every overflow checked first --- */

static Value unsigned_op(Interp *I, int k, int op, Value va, Value vb) {
    uint64_t a = va.as.u, b = vb.as.u, r = 0, max = UMAX[k - IK_U8];
    int ok = 1;
    switch (op) {
    case T_PLUS:
        ok = a <= UINT64_MAX - b;
        if (ok) r = a + b;
        break;
    case T_MINUS:
        ok = a >= b;  /* below zero */
        if (ok) r = a - b;
        break;
    case T_STAR:
        ok = b == 0 || a <= UINT64_MAX / b;
        if (ok) r = a * b;
        break;
    case T_SLASH:
        if (b == 0) return lube_error(I, "division by zero");
        r = a / b;
        break;
    case T_PERCENT:
        if (b == 0) return lube_error(I, "modulo by zero");
        r = a % b;
        break;
    default: {  /* T_POW: int_arith is only given arithmetic operators */
        uint64_t base = a, e = b;
        r = 1;
        while (e > 0 && ok) {
            if (e & 1) {
                ok = base == 0 || r <= max / base;
                if (ok) r *= base;
            }
            e >>= 1;
            if (e > 0 && ok) {
                ok = base == 0 || base <= max / base;
                if (ok) base *= base;
            }
        }
        break;
    }
    }
    if (!ok || r > max) return overflow(I, k, va, op, vb);
    return UINT_VAL(k, r);
}

/* Arithmetic where at least one side is an integer. */
Value int_arith(Interp *I, int op, Value a, Value b) {
    int k;
    if (IS_INT(a) && IS_INT(b)) {
        if (a.ikind != b.ikind)
            return lube_error(I, "cannot mix %s and %s in arithmetic (convert one first, e.g. %s(x))", NAMES[a.ikind],
                              NAMES[b.ikind], NAMES[a.ikind]);
        k = a.ikind;
    } else {
        k = IS_INT(a) ? a.ikind : b.ikind;
        Value *plain = IS_INT(a) ? &b : &a;
        Value conv = from_double(I, plain->as.number, k, 0);
        if (IS_ERR(conv)) return conv;
        *plain = conv;
    }
    return IK_SIGNED(k) ? signed_op(I, k, op, a, b) : unsigned_op(I, k, op, a, b);
}

Value int_negate(Interp *I, Value v) {
    char text[64];
    int k = v.ikind;
    if (IK_SIGNED(k)) {
        if (v.as.i == SMIN[k]) {
            int_format(text, sizeof text, v);
            return lube_error(I, "%s overflow: -(%s) does not fit", NAMES[k], text);
        }
        return SINT_VAL(k, -v.as.i);
    }
    if (v.as.u == 0) return v;
    int_format(text, sizeof text, v);
    return lube_error(I, "cannot negate %s: a %s cannot be below zero", text, NAMES[k]);
}

Value int_abs(Interp *I, Value v) {
    if (IK_SIGNED(v.ikind) && v.as.i < 0) return int_negate(I, v);
    return v;
}
