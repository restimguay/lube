/*
 * money.c - Money: an amount of a currency, exact to its smallest unit.
 *
 *   m(19.99)                 in the default currency (USD until currency("PHP") or another sets it)
 *   m(19.99, "EUR"), m(5, "€"), m("0.10", "PHP")   a currency by its ISO code or its symbol
 *   a + b, a - b, a * 1.07, a / 3, -a, a < b       arithmetic stays exact: a Number joins in the
 *                                                  same currency, and results round half up
 *   total.split(3), total.allocate([2, 1])         shares that add up to the cent
 *   price.to("EUR", 0.92), price.round(0), price.format({...})
 *   price.amount, price.units, price.currency, price.symbol, price.decimals
 *
 * An amount is a count of the currency's smallest unit (cents for USD,
 * yen for JPY, fils for BHD) in 64 bits, so it never drifts the way 0.1 +
 * 0.2 does in floating point. A Number given as an amount counts as the
 * decimal it is written as (m(2.675) is 2.68). Money of two currencies
 * never mixes: .to(code, rate) converts.
 */
#include "lube.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE(name) static Value name(Interp *I, int argc, Value *argv)

/* ======================================================================
 * Currencies: ISO 4217's, and those a program adds
 * ==================================================================== */

typedef struct {
    char code[8];
    char symbol[16];
    int decimals;
} Currency;

/* code, decimals, symbol ("": the code itself) */
static const Currency ISO[] = {
    {"USD", "$", 2}, {"EUR", "\xe2\x82\xac", 2}, {"JPY", "\xc2\xa5", 0}, {"GBP", "\xc2\xa3", 2}, {"CNY", "CN\xc2\xa5", 2},
    {"PHP", "\xe2\x82\xb1", 2}, {"INR", "\xe2\x82\xb9", 2}, {"KRW", "\xe2\x82\xa9", 0}, {"AUD", "A$", 2}, {"CAD", "C$", 2},
    {"CHF", "CHF", 2}, {"HKD", "HK$", 2}, {"SGD", "S$", 2}, {"NZD", "NZ$", 2}, {"TWD", "NT$", 2}, {"MXN", "MX$", 2},
    {"BRL", "R$", 2}, {"RUB", "\xe2\x82\xbd", 2}, {"TRY", "\xe2\x82\xba", 2}, {"THB", "\xe0\xb8\xbf", 2},
    {"VND", "\xe2\x82\xab", 0}, {"IDR", "Rp", 2}, {"MYR", "RM", 2}, {"ZAR", "R", 2}, {"NGN", "\xe2\x82\xa6", 2},
    {"UAH", "\xe2\x82\xb4", 2}, {"ILS", "\xe2\x82\xaa", 2}, {"PLN", "z\xc5\x82", 2}, {"CZK", "K\xc4\x8d", 2},
    {"HUF", "Ft", 2}, {"KZT", "\xe2\x82\xb8", 2}, {"GEL", "\xe2\x82\xbe", 2}, {"AZN", "\xe2\x82\xbc", 2},
    {"CRC", "\xe2\x82\xa1", 2}, {"PYG", "\xe2\x82\xb2", 0}, {"MNT", "\xe2\x82\xae", 2}, {"LAK", "\xe2\x82\xad", 2},
    {"GHS", "\xe2\x82\xb5", 2}, {"EGP", "E\xc2\xa3", 2}, {"ARS", "AR$", 2}, {"DOP", "RD$", 2}, {"JMD", "J$", 2},
    {"TTD", "TT$", 2}, {"XCD", "EC$", 2}, {"UYU", "$U", 2}, {"PEN", "S/", 2}, {"KES", "KSh", 2}, {"XAF", "FCFA", 0},
    {"XOF", "CFA", 0}, {"BDT", "\xe0\xa7\xb3", 2}, {"KHR", "\xe1\x9f\x9b", 2},
    {"AED", "", 2}, {"AFN", "", 2}, {"ALL", "", 2}, {"AMD", "", 2}, {"ANG", "", 2}, {"AOA", "", 2}, {"AWG", "", 2},
    {"BAM", "", 2}, {"BBD", "", 2}, {"BGN", "", 2}, {"BHD", "", 3}, {"BIF", "", 0}, {"BMD", "", 2}, {"BND", "", 2},
    {"BOB", "", 2}, {"BSD", "", 2}, {"BTN", "", 2}, {"BWP", "", 2}, {"BYN", "", 2}, {"BZD", "", 2}, {"CDF", "", 2},
    {"CLP", "", 0}, {"COP", "", 2}, {"CUP", "", 2}, {"CVE", "", 2}, {"DJF", "", 0}, {"DKK", "", 2}, {"DZD", "", 2},
    {"ERN", "", 2}, {"ETB", "", 2}, {"FJD", "", 2}, {"FKP", "", 2}, {"GIP", "", 2}, {"GMD", "", 2}, {"GNF", "", 0},
    {"GTQ", "", 2}, {"GYD", "", 2}, {"HNL", "", 2}, {"HTG", "", 2}, {"IQD", "", 3}, {"IRR", "", 2}, {"ISK", "", 0},
    {"JOD", "", 3}, {"KGS", "", 2}, {"KMF", "", 0}, {"KPW", "", 2}, {"KWD", "", 3}, {"KYD", "", 2}, {"LBP", "", 2},
    {"LKR", "", 2}, {"LRD", "", 2}, {"LSL", "", 2}, {"LYD", "", 3}, {"MAD", "", 2}, {"MDL", "", 2}, {"MGA", "", 2},
    {"MKD", "", 2}, {"MMK", "", 2}, {"MOP", "", 2}, {"MRU", "", 2}, {"MUR", "", 2}, {"MVR", "", 2}, {"MWK", "", 2},
    {"MZN", "", 2}, {"NAD", "", 2}, {"NIO", "", 2}, {"NOK", "", 2}, {"NPR", "", 2}, {"OMR", "", 3}, {"PAB", "", 2},
    {"PGK", "", 2}, {"PKR", "", 2}, {"QAR", "", 2}, {"RON", "", 2}, {"RSD", "", 2}, {"RWF", "", 0}, {"SAR", "", 2},
    {"SBD", "", 2}, {"SCR", "", 2}, {"SDG", "", 2}, {"SEK", "", 2}, {"SHP", "", 2}, {"SLE", "", 2}, {"SOS", "", 2},
    {"SRD", "", 2}, {"SSP", "", 2}, {"STN", "", 2}, {"SVC", "", 2}, {"SYP", "", 2}, {"SZL", "", 2}, {"TJS", "", 2},
    {"TMT", "", 2}, {"TND", "", 3}, {"TOP", "", 2}, {"TZS", "", 2}, {"UGX", "", 0}, {"UZS", "", 2}, {"VES", "", 2},
    {"VUV", "", 0}, {"WST", "", 2}, {"XPF", "", 0}, {"YER", "", 2}, {"ZMW", "", 2}, {"ZWL", "", 2},
};
#define NISO ((int)(sizeof ISO / sizeof ISO[0]))
#define MAX_ADDED 256

static Currency added[MAX_ADDED];  /* add_currency's, appended under the lock (never changed after) */
static int nadded;
static LubeLock *money_lock;

static const Currency *currency_at(int i) { return i < NISO ? &ISO[i] : &added[i - NISO]; }

static const char *symbol_of(const Currency *c) { return c->symbol[0] ? c->symbol : c->code; }

static int same_letters(const char *a, const char *b) {  /* (codes: any case) */
    for (; *a && *b; a++, b++)
        if ((*a >= 'a' && *a <= 'z' ? *a - 32 : *a) != (*b >= 'a' && *b <= 'z' ? *b - 32 : *b)) return 0;
    return !*a && !*b;
}

/* A currency by code (any case) or symbol: its index, or -1. */
static int find_currency(const char *s) {
    lock_acquire(money_lock);
    int n = NISO + nadded, found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (same_letters(currency_at(i)->code, s)) found = i;
    for (int i = 0; i < n && found < 0; i++)
        if (currency_at(i)->symbol[0] && !strcmp(currency_at(i)->symbol, s)) found = i;
    lock_release(money_lock);
    return found;
}

void lube_money_start(void) {
    if (!money_lock) money_lock = lock_new();
}

/* ======================================================================
 * Exact decimal arithmetic on whole numbers of units
 * ==================================================================== */

static const int64_t P10[19] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
                                10000000000LL, 100000000000LL, 1000000000000LL, 10000000000000LL, 100000000000000LL,
                                1000000000000000LL, 10000000000000000LL, 100000000000000000LL, 1000000000000000000LL};

/* A finite double as digits * 10^exp, with the fewest digits (at most 17) that give it back: the decimal it
 * was written as. */
static void decimal_of(double x, int64_t *digits, int *exp) {
    /* The quick way, for most amounts: the fewest decimal places k (up to 8) for which z = x * 10^k rounded
     * gives x back as z / 10^k. While z stays below 2^53/10, decimals k places apart are much farther apart than
     * doubles, so no other one of k places gives x back. */
    int64_t z = 0;
    int k = short_decimal(x, &z);
    if (k >= 0) {
        *digits = z;
        *exp = -k;
        return;
    }
    /* The rest: 15 significant digits, else 16, else 17. (A decimal of at most 15 digits that gives x back is
     * what %.14e makes of x, with zeros after it: 15 digits are farther apart than doubles, but for the
     * subnormals.) */
    char buf[48];
    for (int p = fabs(x) < 2.2250738585072014e-308 ? 1 : 15; p <= 17; p++) {  /* (subnormals: from 1) */
        snprintf(buf, sizeof buf, "%.*e", p - 1, x);
        if (strtod(buf, NULL) == x) break;
    }
    int64_t d = 0;
    int frac = 0, seen_point = 0, neg = 0;
    const char *s = buf;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    for (; *s && *s != 'e'; s++) {
        if (*s == '.') {
            seen_point = 1;
            continue;
        }
        d = d * 10 + (*s - '0');
        frac += seen_point;
    }
    int e = (*s == 'e' ? atoi(s + 1) : 0) - frac;
    while (d && d % 10 == 0) {  /* (15 digits' trailing zeros) */
        d /= 10;
        e++;
    }
    *digits = neg ? -d : d;
    *exp = e;
}

/* An unsigned 128-bit number, for products that don't fit in 64 bits. */
typedef struct {
    uint64_t hi, lo;
} U128;

static U128 mul64(uint64_t a, uint64_t b) {
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    U128 r;
    r.lo = (mid << 32) | (p00 & 0xffffffffu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

/* x * 10, 0 if it would pass 2^127 */
static int times10(U128 *x) {
    if (x->hi >> 59) return 0;
    U128 a = mul64(x->lo, 10);
    a.hi += x->hi * 10;
    *x = a;
    return 1;
}

static int u128_ge(U128 a, U128 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }

/* x / d, the remainder in *rem (d not 0): bit by bit (the uncommon, large cases only). */
static U128 divmod(U128 x, U128 d, U128 *rem) {
    U128 q = {0, 0}, r = {0, 0};
    for (int i = 127; i >= 0; i--) {
        r.hi = (r.hi << 1) | (r.lo >> 63);
        r.lo = (r.lo << 1) | ((i >= 64 ? x.hi >> (i - 64) : x.lo >> i) & 1);
        if (u128_ge(r, d)) {
            uint64_t borrow = r.lo < d.lo;
            r.lo -= d.lo;
            r.hi -= d.hi + borrow;
            if (i >= 64) q.hi |= (uint64_t)1 << (i - 64);
            else q.lo |= (uint64_t)1 << i;
        }
    }
    *rem = r;
    return q;
}

/* n / d rounded half up (away from zero), as a 64-bit magnitude: 0 if it would not fit (*ok cleared). */
static uint64_t div_round(U128 n, U128 d, int *ok) {
    if (!n.hi && !d.hi) {  /* (the usual case: 64 bits, divided at once) */
        uint64_t q = n.lo / d.lo, r = n.lo % d.lo;
        if (r >= d.lo - r) q++;  /* (2r >= d) */
        if (q > (uint64_t)INT64_MAX) {
            *ok = 0;
            return 0;
        }
        return q;
    }
    U128 r, twice;
    U128 q = divmod(n, d, &r);
    twice.hi = (r.hi << 1) | (r.lo >> 63);
    twice.lo = r.lo << 1;
    if (r.hi >> 63 || u128_ge(twice, d)) {  /* 2r >= d: up */
        if (++q.lo == 0) q.hi++;
    }
    if (q.hi || q.lo > (uint64_t)INT64_MAX) {
        *ok = 0;
        return 0;
    }
    return q.lo;
}

/* units * digits * 10^exp, rounded half up (away from zero): 0 with *ok cleared if too large. */
static int64_t scaled(int64_t units, int64_t digits, int exp, int *ok) {
    *ok = 1;
    if (units == 0 || digits == 0) return 0;
    int neg = (units < 0) != (digits < 0);
    uint64_t a = units < 0 ? 0 - (uint64_t)units : (uint64_t)units, b = digits < 0 ? 0 - (uint64_t)digits : (uint64_t)digits;
    U128 p = mul64(a, b), one = {0, 1};
    uint64_t mag;
    if (exp >= 0) {
        for (int i = 0; i < exp; i++)
            if (!times10(&p)) {
                *ok = 0;
                return 0;
            }
        mag = div_round(p, one, ok);
    } else if (exp < -38) {  /* (smaller than half a unit: 0) */
        mag = 0;
    } else {
        U128 d = one;
        for (int i = 0; i < -exp; i++) times10(&d);
        mag = div_round(p, d, ok);
    }
    if (!*ok) return 0;
    return neg ? -(int64_t)mag : (int64_t)mag;
}

/* units / (digits * 10^exp), rounded half up: 0 with *ok cleared if too large. (digits not 0) */
static int64_t divided(int64_t units, int64_t digits, int exp, int *ok) {
    *ok = 1;
    if (units == 0) return 0;
    int neg = (units < 0) != (digits < 0);
    uint64_t a = units < 0 ? 0 - (uint64_t)units : (uint64_t)units, b = digits < 0 ? 0 - (uint64_t)digits : (uint64_t)digits;
    U128 n = {0, a}, d = {0, b};
    for (int i = 0; i < -exp; i++)
        if (!times10(&n)) {
            *ok = 0;
            return 0;
        }
    for (int i = 0; i < exp; i++)
        if (!times10(&d)) return 0;  /* (a divisor past 2^127: the quotient is 0) */
    uint64_t mag = div_round(n, d, ok);
    if (!*ok) return 0;
    return neg ? -(int64_t)mag : (int64_t)mag;
}

/* ======================================================================
 * Money values
 * ==================================================================== */

Value lube_money_new(int64_t units, int cur) {
    ObjMoney *m = (ObjMoney *)alloc_object(sizeof(ObjMoney), OBJ_MONEY);
    m->units = units;
    m->cur = cur;
    return OBJ_VAL(m);
}

static const char *code_of(int cur) { return currency_at(cur)->code; }
static int decimals_of(int cur) { return currency_at(cur)->decimals; }

/* "$1,234.50", "-€0.25", "CHF 5.00", "¥1,235"; with options, another way (format()). (Digits by hand: this is
 * what str() and print() of an amount do.) */
static void money_text(StrBuf *sb, const ObjMoney *m, const char *symbol, int code, const char *thousands, const char *point) {
    const Currency *c = currency_at(m->cur);
    uint64_t mag = m->units < 0 ? 0 - (uint64_t)m->units : (uint64_t)m->units;
    uint64_t whole = mag / (uint64_t)P10[c->decimals], frac = mag % (uint64_t)P10[c->decimals];
    char digits[24];
    int n = 0;
    do {  /* (backwards) */
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    if (m->units < 0) sb_putc(sb, '-');
    if (code) {
        sb_puts(sb, c->code);
        sb_putc(sb, ' ');
    } else if (*symbol) {  /* a space after one that ends with a letter: "CHF 5.00", "zł 5.00"; not "$5.00" */
        size_t len = strlen(symbol);
        unsigned char last = (unsigned char)symbol[len - 1], lead = len > 1 ? (unsigned char)symbol[len - 2] : 0;
        int letter = (last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z') || lead == 0xC4 || lead == 0xC5;
        sb_puts(sb, symbol);
        if (letter) sb_putc(sb, ' ');
    }
    for (int i = n - 1; i >= 0; i--) {
        sb_putc(sb, digits[i]);
        if (i && i % 3 == 0) sb_puts(sb, thousands);
    }
    if (c->decimals) {
        char f[24];
        for (int i = c->decimals - 1; i >= 0; i--) {
            f[i] = (char)('0' + frac % 10);
            frac /= 10;
        }
        f[c->decimals] = '\0';
        sb_puts(sb, point);
        sb_puts(sb, f);
    }
}

void lube_money_describe(StrBuf *sb, Value v) {
    const ObjMoney *m = AS_MONEY(v);
    money_text(sb, m, symbol_of(currency_at(m->cur)), 0, ",", ".");
}

/* A Number as units of currency cur (rounded half up to the smallest unit): 0 after an error. */
static int units_of_number(Interp *I, double x, int cur, int64_t *out) {
    if (!isfinite(x)) {
        lube_error(I, "money cannot be %s", x != x ? "nan" : "infinite");
        return 0;
    }
    int64_t d;
    int e, ok;
    decimal_of(x, &d, &e);
    *out = scaled(1, d, e + decimals_of(cur), &ok);
    if (!ok) lube_error(I, "%g %s is too much money (an amount holds at most 2^63 of its smallest unit)", x, code_of(cur));
    return ok;
}

/* "19.99", "-1,234.5", "0.125": as units of cur; 0 after an error. */
static int units_of_text(Interp *I, const char *s, int cur, int64_t *out) {
    const char *p = s;
    int neg = 0, digits = 0, frac = -1;
    int64_t d = 0;
    while (*p == ' ') p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    for (; *p; p++) {
        if (*p == ',' && frac < 0 && digits) continue;
        if (*p == '.' && frac < 0) {
            frac = 0;
            continue;
        }
        if (*p < '0' || *p > '9') break;
        if (digits == 18) {
            lube_error(I, "m(): \"%s\" has more digits than money holds (18)", s);
            return 0;
        }
        d = d * 10 + (*p - '0');
        digits++;
        if (frac >= 0) frac++;
    }
    while (*p == ' ') p++;
    if (!digits || *p) {
        lube_error(I, "m(): \"%s\" is not an amount (write it as 19.99)", s);
        return 0;
    }
    int ok;
    *out = scaled(1, neg ? -d : d, decimals_of(cur) - (frac < 0 ? 0 : frac), &ok);
    if (!ok) lube_error(I, "m(): %s %s is too much money (an amount holds at most 2^63 of its smallest unit)", s, code_of(cur));
    return ok;
}

/* A currency argument (a code or a symbol): its index, or -1 after an error. */
static int currency_arg(Interp *I, const char *fn, Value v) {
    if (!IS_TEXT(v)) {
        lube_error(I, "%s() takes a currency as a str (a code such as \"USD\", or a symbol such as \"$\"), not %s", fn, a_type(v));
        return -1;
    }
    int cur = find_currency(text_of(v)->chars);
    if (cur < 0) lube_error(I, "%s(): there is no currency '%s' (add_currency() adds one)", fn, text_of(v)->chars);
    return cur;
}

/* m(amount?, currency?) and Money(amount?, currency?) */
NATIVE(n_m) {
    int cur = argc > 1 ? currency_arg(I, "m", argv[1]) : I->currency;
    if (cur < 0) return ERR_VAL;
    if (argc == 0) return lube_money_new(0, cur);
    Value a = argv[0];
    int64_t units;
    if (IS_MONEY(a)) {
        if (argc > 1 && AS_MONEY(a)->cur != cur)
            return lube_error(I, "m(): %s is already in %s: change its currency with .to(\"%s\", rate)", "this money",
                              code_of(AS_MONEY(a)->cur), code_of(cur));
        return retain(a);
    }
    if (IS_INT(a)) {
        if (!IK_SIGNED(a.ikind) && a.as.u > (uint64_t)INT64_MAX) return lube_error(I, "m(): that is too much money");
        int64_t whole = IK_SIGNED(a.ikind) ? a.as.i : (int64_t)a.as.u;
        int ok;
        units = scaled(1, whole, decimals_of(cur), &ok);
        if (!ok) return lube_error(I, "m(): that is too much money (an amount holds at most 2^63 of its smallest unit)");
    } else if (IS_NUM(a)) {
        if (!units_of_number(I, a.as.number, cur, &units)) return ERR_VAL;
    } else if (IS_TEXT(a)) {
        if (!units_of_text(I, text_of(a)->chars, cur, &units)) return ERR_VAL;
    } else {
        return lube_error(I, "m() takes an amount (a Number, or text such as \"19.99\"), not %s", a_type(a));
    }
    return lube_money_new(units, cur);
}

/* currency(): the default currency's code; currency(code or symbol): sets it (for m() without one) */
NATIVE(n_currency) {
    if (argc == 0) return OBJ_VAL(new_string(code_of(I->currency), strlen(code_of(I->currency))));
    int cur = currency_arg(I, "currency", argv[0]);
    if (cur < 0) return ERR_VAL;
    I->currency = cur;
    return NIL_VAL;
}

/* add_currency(code, symbol, decimals): a currency of one's own (BTC, game coins...) */
NATIVE(n_add_currency) {
    (void)argc;
    if (!IS_TEXT(argv[0]) || !IS_TEXT(argv[1]) || !IS_NUMERIC(argv[2]))
        return lube_error(I, "add_currency() takes a code, a symbol (str) and its number of decimals");
    const char *code = text_of(argv[0])->chars, *symbol = text_of(argv[1])->chars;
    double dec = numeric_value(argv[2]);
    size_t cl = text_len(argv[0]), sl = text_len(argv[1]);
    int good = cl > 0 && cl < sizeof added[0].code && sl < sizeof added[0].symbol;
    for (size_t i = 0; i < cl && good; i++) good = (code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= 'a' && code[i] <= 'z') || (code[i] >= '0' && code[i] <= '9');
    if (!good) return lube_error(I, "add_currency(): a code is 1 to 7 letters or digits, and a symbol at most 15 bytes");
    if (dec != floor(dec) || dec < 0 || dec > 9) return lube_error(I, "add_currency(): decimals is a whole number from 0 to 9");
    int have = find_currency(code);
    if (have >= 0) {
        const Currency *c = currency_at(have);
        if (c->decimals == (int)dec && !strcmp(symbol_of(c), sl ? symbol : code)) return NIL_VAL;  /* (the same again) */
        return lube_error(I, "add_currency(): %s is already a currency, with %d decimals and symbol %s", c->code, c->decimals, symbol_of(c));
    }
    lock_acquire(money_lock);
    if (nadded == MAX_ADDED) {
        lock_release(money_lock);
        return lube_error(I, "add_currency(): no more than %d currencies can be added", MAX_ADDED);
    }
    Currency *c = &added[nadded];
    memcpy(c->code, code, cl + 1);
    for (char *p = c->code; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    memcpy(c->symbol, symbol, sl + 1);
    c->decimals = (int)dec;
    nadded++;
    lock_release(money_lock);
    return NIL_VAL;
}

/* ======================================================================
 * Operators
 * ==================================================================== */

static const char *op_text(int op) {
    switch (op) {
    case T_PLUS: return "add";
    case T_MINUS: return "subtract";
    default: return "compare";
    }
}

/* A Number (or sized integer) as money of currency cur; 0 after an error. */
static int as_units(Interp *I, Value v, int cur, int64_t *out) {
    if (IS_MONEY(v)) {
        *out = AS_MONEY(v)->units;
        return 1;
    }
    return units_of_number(I, numeric_value(v), cur, out);
}

static Value mismatch(Interp *I, int op, Value a, Value b) {
    StrBuf x, y;
    sb_init(&x);
    sb_init(&y);
    lube_money_describe(&x, a);
    lube_money_describe(&y, b);
    Value e = lube_error(I, "cannot %s %s and %s: they are different currencies (convert one with .to(code, rate))",
                         op_text(op), x.data, y.data);
    sb_free(&x);
    sb_free(&y);
    return e;
}

static int64_t checked(Interp *I, int64_t a, int64_t b, int sub, int *ok) {
    int64_t r;
    if (sub ? (b < 0 ? a > INT64_MAX + b : a < INT64_MIN + b) : (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)) {
        *ok = 0;
        lube_error(I, "the result is too much money");
        return 0;
    }
    r = sub ? a - b : a + b;
    *ok = 1;
    return r;
}

/* a op b where a or b is Money (op T_PLUS... from the interpreter); 0 if it is not money's to answer. */
int lube_money_op(Interp *I, int op, Value a, Value b, Value *out) {
    int am = IS_MONEY(a);
    Value money = am ? a : b, other = am ? b : a;
    int cur = AS_MONEY(money)->cur;
    int other_num = IS_NUMERIC(other), other_money = IS_MONEY(other);
    if (!other_num && !other_money) {
        if (op == T_EQ || op == T_NEQ) return 0;  /* (values_equal: not equal) */
        if (op == T_PLUS && IS_TEXT(other)) return 0;  /* (joined as text) */
        *out = lube_error(I, "cannot use %s with %s and %s", op == T_PLUS ? "+" : op == T_MINUS ? "-" : op == T_STAR ? "*" : op == T_SLASH ? "/" : op == T_PERCENT ? "%" : op == T_POW ? "**" : "<", a_type(a), a_type(b));
        return 1;
    }
    if (other_money && AS_MONEY(other)->cur != cur) {
        if (op == T_EQ || op == T_NEQ) {
            *out = BOOL_VAL(op == T_NEQ);
            return 1;
        }
        if (op == T_PLUS || op == T_MINUS || op == T_LT || op == T_LE || op == T_GT || op == T_GE || op == T_PERCENT || op == T_SLASH) {
            *out = mismatch(I, op, a, b);
            return 1;
        }
    }
    int64_t x, y;
    int ok;
    switch (op) {
    case T_PLUS: case T_MINUS:
        if (!as_units(I, a, cur, &x) || !as_units(I, b, cur, &y)) {
            *out = ERR_VAL;
            return 1;
        }
        x = checked(I, x, y, op == T_MINUS, &ok);
        *out = ok ? lube_money_new(x, cur) : ERR_VAL;
        return 1;
    case T_STAR:
        if (other_money) {
            *out = lube_error(I, "cannot multiply money by money (multiply by a Number)");
            return 1;
        } else {
            double k = numeric_value(other);
            if (!isfinite(k)) {
                *out = lube_error(I, "cannot multiply money by %s", k != k ? "nan" : "infinity");
                return 1;
            }
            int64_t d;
            int e;
            decimal_of(k, &d, &e);
            x = scaled(AS_MONEY(money)->units, d, e, &ok);
            *out = ok ? lube_money_new(x, cur) : lube_error(I, "the result is too much money");
            return 1;
        }
    case T_SLASH:
        if (!am) {
            *out = lube_error(I, "cannot divide a Number by money");
            return 1;
        }
        if (other_money) {  /* how many times: a Number */
            if (!AS_MONEY(other)->units) {
                *out = lube_error(I, "division by zero");
                return 1;
            }
            *out = NUM_VAL((double)AS_MONEY(a)->units / (double)AS_MONEY(other)->units);
            return 1;
        } else {
            double k = numeric_value(other);
            if (k == 0) {
                *out = lube_error(I, "division by zero");
                return 1;
            }
            if (!isfinite(k)) {
                *out = k != k ? lube_error(I, "cannot divide money by nan") : lube_money_new(0, cur);
                return 1;
            }
            int64_t d;
            int e;
            decimal_of(k, &d, &e);
            x = divided(AS_MONEY(a)->units, d, e, &ok);
            *out = ok ? lube_money_new(x, cur) : lube_error(I, "the result is too much money");
            return 1;
        }
    case T_PERCENT:
        if (!am) {
            *out = lube_error(I, "cannot take a Number modulo money");
            return 1;
        }
        if (!as_units(I, b, cur, &y)) {
            *out = ERR_VAL;
            return 1;
        }
        if (y == 0) {
            *out = lube_error(I, "modulo by zero");
            return 1;
        }
        x = AS_MONEY(a)->units;
        x = y == -1 ? 0 : x % y;
        if (x != 0 && ((x < 0) != (y < 0))) x += y;  /* (the sign of the divisor, as Lube's %) */
        *out = lube_money_new(x, cur);
        return 1;
    case T_LT: case T_LE: case T_GT: case T_GE: case T_EQ: case T_NEQ:
        if (!as_units(I, a, cur, &x) || !as_units(I, b, cur, &y)) {
            *out = ERR_VAL;
            return 1;
        }
        *out = BOOL_VAL(op == T_LT ? x < y : op == T_LE ? x <= y : op == T_GT ? x > y : op == T_GE ? x >= y
                        : op == T_EQ ? x == y : x != y);
        return 1;
    default:  /* T_POW */
        *out = lube_error(I, "cannot raise money to a power");
        return 1;
    }
}

Value lube_money_negate(Interp *I, Value v) {
    if (AS_MONEY(v)->units == INT64_MIN) return lube_error(I, "the result is too much money");  /* LCOV_EXCL_LINE */
    return lube_money_new(-AS_MONEY(v)->units, AS_MONEY(v)->cur);
}

/* For sort, min and max: -1, 0 or 1; *failed (with an error) for two currencies. */
int lube_money_compare(Interp *I, Value a, Value b, const char *fn, int *failed) {
    if (AS_MONEY(a)->cur != AS_MONEY(b)->cur) {
        mismatch(I, T_LT, a, b);
        (void)fn;
        *failed = 1;
        return 0;
    }
    int64_t x = AS_MONEY(a)->units, y = AS_MONEY(b)->units;
    return x < y ? -1 : x > y;
}

/* ======================================================================
 * Properties and methods
 * ==================================================================== */

/* m.amount, m.units, m.currency, m.symbol, m.decimals */
Value lube_money_field(Interp *I, Value v, ObjString *name) {
    const ObjMoney *m = AS_MONEY(v);
    const Currency *c = currency_at(m->cur);
    if (!strcmp(name->chars, "amount")) return NUM_VAL((double)m->units / (double)P10[c->decimals]);
    if (!strcmp(name->chars, "units")) return NUM_VAL((double)m->units);
    if (!strcmp(name->chars, "currency")) return OBJ_VAL(new_string(c->code, strlen(c->code)));
    if (!strcmp(name->chars, "symbol")) return OBJ_VAL(new_string(symbol_of(c), strlen(symbol_of(c))));
    if (!strcmp(name->chars, "decimals")) return NUM_VAL(c->decimals);
    return lube_error(I, "Money has no field '%s' (it has amount, units, currency, symbol and decimals)", name->chars);
}

/* allocate(ratios): shares in those proportions that add up exactly (the units left over go to the shares
 * that lost the most to rounding: the largest remainders) */
static Value allocate(Interp *I, const ObjMoney *m, const double *ratios, size_t n) {
    int64_t *r = xmalloc(sizeof(int64_t) * (n ? n : 1));
    int emin = 0, first = 1;
    int64_t dig;
    int e;
    for (size_t i = 0; i < n; i++) {  /* the ratios as whole numbers: one scale for all */
        decimal_of(ratios[i], &dig, &e);
        if (dig && (first || e < emin)) {
            emin = e;
            first = 0;
        }
    }
    U128 total = {0, 0};
    int fits = 1;
    for (size_t i = 0; i < n && fits; i++) {
        decimal_of(ratios[i], &dig, &e);
        int ok;
        r[i] = dig ? scaled(1, dig, e - emin, &ok) : 0;
        if (dig && (!ok || r[i] > (int64_t)1 << 62)) fits = 0;
        uint64_t lo = total.lo + (uint64_t)r[i];
        total.hi += lo < total.lo;
        total.lo = lo;
        if (total.hi || total.lo > (uint64_t)1 << 62) fits = 0;
    }
    if (!fits) {  /* (ratios too far apart to scale together: by doubles, in 2^52 parts) */
        double t = 0;
        for (size_t i = 0; i < n; i++) t += ratios[i];
        total.hi = total.lo = 0;
        for (size_t i = 0; i < n; i++) {
            r[i] = (int64_t)(ratios[i] / t * 4503599627370496.0);
            total.lo += (uint64_t)r[i];
        }
    }
    uint64_t mag = m->units < 0 ? 0 - (uint64_t)m->units : (uint64_t)m->units, given = 0;
    uint64_t *share = xmalloc(sizeof(uint64_t) * (n ? n : 1));
    U128 *rem = xmalloc(sizeof(U128) * (n ? n : 1));
    char *taken = xmalloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) {
        U128 q = divmod(mul64(mag, (uint64_t)r[i]), total, &rem[i]);
        share[i] = q.lo;
        given += q.lo;
        taken[i] = 0;
    }
    for (uint64_t left = mag - given; left > 0; left--) {  /* (fewer than n: one each, largest remainder first) */
        size_t best = n;
        for (size_t i = 0; i < n; i++)
            if (!taken[i] && (best == n || rem[i].hi > rem[best].hi || (rem[i].hi == rem[best].hi && rem[i].lo > rem[best].lo)))
                best = i;
        share[best]++;
        taken[best] = 1;
    }
    ObjList *out = new_list();
    for (size_t i = 0; i < n; i++)
        list_push(out, lube_money_new(m->units < 0 ? -(int64_t)share[i] : (int64_t)share[i], m->cur));
    free(r);
    free(share);
    free(rem);
    free(taken);
    (void)I;
    return OBJ_VAL(out);
}

NATIVE(n_allocate) {
    (void)argc;
    if (!IS_LIST(argv[1])) return lube_error(I, "allocate() takes a list of ratios (Numbers)");
    ObjList *l = AS_LIST(argv[1]);
    double *ratios = xmalloc(sizeof(double) * (l->count ? l->count : 1)), total = 0;
    for (size_t i = 0; i < l->count; i++) {
        double x = IS_NUMERIC(l->items[i]) ? numeric_value(l->items[i]) : -1;
        if (!(x >= 0) || !isfinite(x)) {
            free(ratios);
            return lube_error(I, "allocate(): ratios are Numbers, 0 or more");
        }
        ratios[i] = x;
        total += x;
    }
    if (!(total > 0)) {
        free(ratios);
        return lube_error(I, "allocate(): the ratios must add up to more than 0");
    }
    Value r = allocate(I, AS_MONEY(argv[0]), ratios, l->count);
    free(ratios);
    return r;
}

/* split(n): n shares as equal as can be (the first ones a unit more) */
NATIVE(n_split) {
    (void)argc;
    double n = IS_NUMERIC(argv[1]) ? numeric_value(argv[1]) : 0;
    if (n < 1 || n != floor(n) || n > 1e7) return lube_error(I, "split() takes a whole number of shares, 1 or more");
    size_t k = (size_t)n;
    const ObjMoney *m = AS_MONEY(argv[0]);
    int64_t base = m->units / (int64_t)k, extra = m->units % (int64_t)k;
    ObjList *out = new_list();
    for (size_t i = 0; i < k; i++)
        list_push(out, lube_money_new(base + (extra > 0 && (int64_t)i < extra ? 1 : extra < 0 && (int64_t)i < -extra ? -1 : 0), m->cur));
    return OBJ_VAL(out);
}

/* to(currency, rate): in another currency, at rate (of it per one of this), rounded half up */
NATIVE(n_to) {
    (void)argc;
    int cur = currency_arg(I, "to", argv[1]);
    if (cur < 0) return ERR_VAL;
    double rate = IS_NUMERIC(argv[2]) ? numeric_value(argv[2]) : -1;
    if (!(rate > 0) || !isfinite(rate)) return lube_error(I, "to(): the rate is a Number, more than 0");
    const ObjMoney *m = AS_MONEY(argv[0]);
    int64_t d;
    int e, ok;
    decimal_of(rate, &d, &e);
    int64_t units = scaled(m->units, d, e + decimals_of(cur) - decimals_of(m->cur), &ok);
    return ok ? lube_money_new(units, cur) : lube_error(I, "the result is too much money");
}

/* round(decimals?): to that many decimals (0 unless given: whole dollars, euros...), half up */
NATIVE(n_mround) {
    const ObjMoney *m = AS_MONEY(argv[0]);
    int dec = decimals_of(m->cur);
    double want = argc > 1 && IS_NUMERIC(argv[1]) ? numeric_value(argv[1]) : argc > 1 ? -1 : 0;
    if (want < 0 || want != floor(want)) return lube_error(I, "round() takes a whole number of decimals, 0 or more");
    if (want >= dec) return retain(argv[0]);
    int ok;
    int64_t units = scaled(m->units, 1, -(dec - (int)want), &ok);  /* (smaller: it fits) */
    (void)ok;
    return lube_money_new(units * P10[dec - (int)want], m->cur);
}

/* format(options): {symbol (true, false or one's own), code (true: "USD 5.00"), thousands (","), decimal (".")} */
NATIVE(n_format) {
    const ObjMoney *m = AS_MONEY(argv[0]);
    const char *symbol = symbol_of(currency_at(m->cur)), *thousands = ",", *point = ".";
    int code = 0;
    Value held[3];  /* (the options' texts, kept here while their bytes are read: a short str's are in the value) */
    size_t n;
    if (argc > 1) {
        if (!IS_MAP(argv[1])) return lube_error(I, "format() takes options: {symbol, code, thousands, decimal}");
        ObjMap *o = AS_MAP(argv[1]);
        Value v;
        if (map_get(o, OBJ_VAL(lube_intern(I, "symbol", 6)), &v)) {
            if (IS_BOOL(v)) symbol = v.as.boolean ? symbol : "";
            else if (IS_TEXT(v)) symbol = (held[0] = v, text_chars(&held[0], &n));
            else return lube_error(I, "format(): symbol is true, false or a str");
        }
        if (map_get(o, OBJ_VAL(lube_intern(I, "code", 4)), &v)) {
            if (!IS_BOOL(v)) return lube_error(I, "format(): code is true or false");
            code = v.as.boolean;
        }
        if (map_get(o, OBJ_VAL(lube_intern(I, "thousands", 9)), &v)) {
            if (!IS_TEXT(v)) return lube_error(I, "format(): thousands is a str");
            thousands = (held[1] = v, text_chars(&held[1], &n));
        }
        if (map_get(o, OBJ_VAL(lube_intern(I, "decimal", 7)), &v)) {
            if (!IS_TEXT(v)) return lube_error(I, "format(): decimal is a str");
            point = (held[2] = v, text_chars(&held[2], &n));
        }
    }
    StrBuf sb;
    sb_init(&sb);
    money_text(&sb, m, symbol, code, thousands, point);
    ObjString *s = new_string(sb.data, sb.len);
    sb_free(&sb);
    return OBJ_VAL(s);
}

/* ======================================================================
 * Tables
 * ==================================================================== */

static const LubeNativeDef MONEY[] = {
    LUBE_BUILTIN(m, n_m, 0, 2, 0)
    LUBE_BUILTIN(Money, n_m, 0, 2, 0)
    LUBE_BUILTIN(currency, n_currency, 0, 1, 0)
    LUBE_BUILTIN(add_currency, n_add_currency, 3, 3, 0)
    {NULL, NULL, 0, 0, 0}
};

static const LubeNativeDef MONEY_METHODS[] = {  /* (the receiver is argument 0) */
    LUBE_METHOD(allocate, n_allocate, 2, 2, 0)
    LUBE_METHOD(split, n_split, 2, 2, 0)
    LUBE_METHOD(to, n_to, 3, 3, 0)
    LUBE_METHOD(round, n_mround, 1, 2, 0)
    LUBE_METHOD(format, n_format, 1, 2, 0)
    {NULL, NULL, 0, 0, 0}
};

void register_money(Interp *I) {
    lube_money_start();
    lube_define_natives(I, MONEY);
    I->tmethods[OBJ_MONEY - OBJ_BYTES] = lube_method_table(I, MONEY_METHODS);
}
