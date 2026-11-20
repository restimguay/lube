/*
 * random.c - the plugin random: Python's random module, number for number.
 *
 *   import random                       (or import random.*: its names, bare)
 *   random.seed(42)                     then random.random() is 0.6394267984578837,
 *   print(random.random())              as it is in Python
 *   random(), uniform(a, b), triangular(low?, high?, mode?), randint(a, b),
 *   randrange(start, stop?, step?), getrandbits(k), randbytes(n), choice(xs),
 *   choices(xs, weights?, {weights, cum_weights, k}), sample(xs, k, {counts}),
 *   shuffle(xs) (a new list), gauss, normalvariate, lognormvariate,
 *   expovariate, vonmisesvariate, gammavariate, betavariate, paretovariate,
 *   weibullvariate, binomialvariate, seed(a?, version?), getstate(), setstate(s)
 *   Random(seed?)                       a generator of its own: r.random(), r.choice(xs)...
 *   SystemRandom()                      the system's random bytes: no seed, no state
 *   BPF, RECIP_BPF, LOG4, NV_MAGICCONST, SG_MAGICCONST, TWOPI
 *
 * The generator is Python's: the Mersenne Twister (MT19937), seeded as
 * Python seeds it (a whole Number as an int, another as a float, text and
 * bytes through SHA-512), and every function draws from it as Python's
 * does, so a seed gives the numbers it gives in Python. The package's
 * functions share one generator, under a lock: isolates can call them at
 * once. The plugin statistics draws from it too (see RandomGives).
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_RAND_S  /* rand_s: the system's random bytes */
#endif
#include "../lube_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __clang__
#pragma STDC FP_CONTRACT OFF  /* a * b + c rounded twice, as Python does it: never one fma */
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "r_randint" -> "randint" */
#define NEED(cond, i, what)                                                                                    \
    do {                                                                                                       \
        if (!(cond))                                                                                           \
            return lube->error(vm, "%s() expects %s as argument %d, got %s", FNAME, (what), (i) + 1 + base,   \
                               lube->type_name(argv[i]));                                                      \
    } while (0)
#define NEED_NUM(i) NEED(lube->kind(argv[i]) == LUBE_NUMBER, i, "a Number")
#define NUM(i) lube->number(argv[i])
#define NUM_OR(i, dflt) (argc > (i) ? NUM(i) : (dflt))
/* argv[i], a whole Number (up to 2^53 either way), into var */
#define WHOLE(i, var)                                                                                          \
    do {                                                                                                       \
        if (!whole_ok(argv[i], &(var))) return not_whole(vm, FNAME, argv[i], (i) + 1 + base);                  \
    } while (0)

#define MT_N 624
#define MAX_WHOLE 9007199254740992.0  /* 2^53: past it, a Number's whole values have gaps */
#define BPF 53
#define RECIP_BPF (1.0 / 9007199254740992.0)
#define LOG4 1.3862943611198906            /* log(4.0) */
#define NV_MAGICCONST 1.7155277699214135   /* 4 * exp(-0.5) / sqrt(2.0) */
#define SG_MAGICCONST 2.504077396776274    /* 1.0 + log(4.5) */
#define TWOPI 6.283185307179586
#define PI 3.141592653589793
#define EULER 2.718281828459045

static const LubeAPI *lube;
static LubeType *RandomType, *SystemType;
static LubeLock *lock;  /* over shared_gen */

typedef struct {
    uint32_t mt[MT_N];
    int index;           /* where the next word is: at MT_N, the next draw makes MT_N more */
    int has_gauss;       /* gauss() made two numbers, and the second waits in gauss_next */
    double gauss_next;
    int system;          /* a SystemRandom: draws the system's random bytes, and has no state */
} Gen;

static Gen shared_gen;  /* the package's (under the lock) */

/* ======================================================================
 * The Mersenne Twister, as Python's _random has it
 * ==================================================================== */

static void init_genrand(Gen *g, uint32_t s) {
    g->mt[0] = s;
    for (int i = 1; i < MT_N; i++) g->mt[i] = 1812433253u * (g->mt[i - 1] ^ (g->mt[i - 1] >> 30)) + (uint32_t)i;
    g->index = MT_N;
}

static void init_by_array(Gen *g, const uint32_t *key, size_t len) {
    uint32_t *mt = g->mt;
    init_genrand(g, 19650218u);
    size_t i = 1, j = 0, k = MT_N > len ? MT_N : len;
    for (; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
        i++;
        j++;
        if (i >= MT_N) {
            mt[0] = mt[MT_N - 1];
            i = 1;
        }
        if (j >= len) j = 0;
    }
    for (k = MT_N - 1; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
        i++;
        if (i >= MT_N) {
            mt[0] = mt[MT_N - 1];
            i = 1;
        }
    }
    mt[0] = 0x80000000u;  /* (so the state is never all zeros) */
    g->has_gauss = 0;
}

static void refill(Gen *g) {
    static const uint32_t mag01[2] = {0, 0x9908b0dfu};
    uint32_t *mt = g->mt, y;
    int k;
    for (k = 0; k < MT_N - 397; k++) {
        y = (mt[k] & 0x80000000u) | (mt[k + 1] & 0x7fffffffu);
        mt[k] = mt[k + 397] ^ (y >> 1) ^ mag01[y & 1];
    }
    for (; k < MT_N - 1; k++) {
        y = (mt[k] & 0x80000000u) | (mt[k + 1] & 0x7fffffffu);
        mt[k] = mt[k + 397 - MT_N] ^ (y >> 1) ^ mag01[y & 1];
    }
    y = (mt[MT_N - 1] & 0x80000000u) | (mt[0] & 0x7fffffffu);
    mt[MT_N - 1] = mt[396] ^ (y >> 1) ^ mag01[y & 1];
    g->index = 0;
}

static uint32_t next32(Gen *g) {
    if (g->index >= MT_N) refill(g);
    uint32_t y = g->mt[g->index++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

/* ======================================================================
 * The system's random bytes
 * ==================================================================== */

/* Fills buf with the system's random bytes: 0 if it has none. */
static int system_bytes(void *buf, size_t n) {
#if defined(_WIN32)
    unsigned char *p = (unsigned char *)buf;
    while (n) {
        unsigned int v;
        if (rand_s(&v)) return 0;
        size_t take = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, take);
        p += take;
        n -= take;
    }
    return 1;
#elif defined(__APPLE__)
    arc4random_buf(buf, n);
    return 1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

/* seed(): from the system's random bytes, and from the time and where things are, should it have none. */
static void seed_system(Gen *g) {
    uint32_t key[MT_N];
    memset(key, 0, sizeof key);
    system_bytes(key, sizeof key);
    uint64_t t = (uint64_t)time(NULL), c = (uint64_t)clock(), at = (uint64_t)(uintptr_t)g;
    double now = lube->now();
    uint64_t now_bits;
    memcpy(&now_bits, &now, sizeof now_bits);
    uint64_t mix[4] = {t, c, now_bits, at ^ (uint64_t)(uintptr_t)key};
    for (int i = 0; i < 4; i++) {
        key[2 * i] ^= (uint32_t)mix[i];
        key[2 * i + 1] ^= (uint32_t)(mix[i] >> 32);
    }
    init_by_array(g, key, MT_N);
}

/* A big-endian number of n bytes (n up to 8) of the system's. */
static uint64_t system_number(int n) {
    unsigned char b[8];
    uint64_t x = 0;
    system_bytes(b, (size_t)n);
    for (int i = 0; i < n; i++) x = x << 8 | b[i];
    return x;
}

/* ======================================================================
 * Drawing: random(), getrandbits(k) and _randbelow(n)
 * ==================================================================== */

/* random(): 53 random bits, from 0 up to (not including) 1 */
static double draw(Gen *g) {
    if (g->system) return (double)(system_number(7) >> 3) * RECIP_BPF;
    uint32_t a = next32(g) >> 5, b = next32(g) >> 6;
    return (a * 67108864.0 + b) * RECIP_BPF;
}

/* getrandbits(k), k up to 64 */
static uint64_t bits(Gen *g, int k) {
    if (k == 0) return 0;
    if (g->system) {
        int n = (k + 7) / 8;
        return system_number(n) >> (n * 8 - k);
    }
    if (k <= 32) return next32(g) >> (32 - k);
    uint64_t lo = next32(g);
    uint32_t hi = next32(g);
    if (k < 64) hi >>= 64 - k;
    return lo | (uint64_t)hi << 32;
}

/* _randbelow(n): from 0 to n - 1 (n at least 1), by getrandbits and trying again */
static uint64_t below(Gen *g, uint64_t n) {
    int k = 0;
    while (k < 64 && (n >> k) != 0) k++;
    uint64_t r = bits(g, k);
    while (r >= n) r = bits(g, k);
    return r;
}

/* ======================================================================
 * Seeding, as Python's random.seed(a, version)
 * ==================================================================== */

/* SHA-512 (FIPS 180-4): text and bytes seeds go through it, as in Python. */
static const uint64_t K512[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd), UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc),
    UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019), UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118),
    UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe), UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1), UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694),
    UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3), UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65),
    UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483), UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210), UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4),
    UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725), UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70),
    UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926), UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8), UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b),
    UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001), UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30),
    UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910), UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53), UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8),
    UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb), UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3),
    UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60), UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9), UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b),
    UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207), UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178),
    UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6), UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493), UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c),
    UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a), UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817),
};

#define ROTR(x, n) ((x) >> (n) | (x) << (64 - (n)))

static void sha512_block(uint64_t h[8], const unsigned char *p) {
    uint64_t w[80], v[8];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int b = 0; b < 8; b++) w[i] = w[i] << 8 | p[8 * i + b];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROTR(w[i - 15], 1) ^ ROTR(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROTR(w[i - 2], 19) ^ ROTR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 80; i++) {
        uint64_t s1 = ROTR(v[4], 14) ^ ROTR(v[4], 18) ^ ROTR(v[4], 41);
        uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint64_t t1 = v[7] + s1 + ch + K512[i] + w[i];
        uint64_t s0 = ROTR(v[0], 28) ^ ROTR(v[0], 34) ^ ROTR(v[0], 39);
        uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + s0 + maj;
    }
    for (int i = 0; i < 8; i++) h[i] += v[i];
}

static void sha512(const unsigned char *data, size_t len, unsigned char out[64]) {
    uint64_t h[8] = {UINT64_C(0x6a09e667f3bcc908), UINT64_C(0xbb67ae8584caa73b), UINT64_C(0x3c6ef372fe94f82b),
                     UINT64_C(0xa54ff53a5f1d36f1), UINT64_C(0x510e527fade682d1), UINT64_C(0x9b05688c2b3e6c1f),
                     UINT64_C(0x1f83d9abfb41bd6b), UINT64_C(0x5be0cd19137e2179)};
    size_t done = 0;
    for (; len - done >= 128; done += 128) sha512_block(h, data + done);
    unsigned char last[256] = {0};
    size_t rest = len - done, end = rest < 112 ? 128 : 256;
    if (rest) memcpy(last, data + done, rest);
    last[rest] = 0x80;
    uint64_t bit_len = (uint64_t)len << 3;  /* (its top 64 bits: len >> 61, zero for any length a str can have) */
    for (int b = 0; b < 8; b++) last[end - 1 - b] = (unsigned char)(bit_len >> (8 * b));
    for (size_t at = 0; at < end; at += 128) sha512_block(h, last + at);
    for (int i = 0; i < 64; i++) out[i] = (unsigned char)(h[i / 8] >> (56 - 8 * (i % 8)));
}

/* Python's hash of a float that is not whole (or infinite), which it seeds with, as an unsigned number. */
static uint64_t hash_double(double v) {
    const uint64_t P = (UINT64_C(1) << 61) - 1;
    if (isinf(v)) return v > 0 ? UINT64_C(314159) : (uint64_t)0 - UINT64_C(314159);
    int e, negative = 0;
    double m = frexp(v, &e);
    if (m < 0) {
        negative = 1;
        m = -m;
    }
    uint64_t x = 0;
    while (m) {
        x = ((x << 28) & P) | x >> (61 - 28);
        m *= 268435456.0;  /* 2^28 */
        e -= 28;
        uint64_t y = (uint64_t)m;
        m -= (double)y;
        x += y;
        if (x >= P) x -= P;
    }
    e = e >= 0 ? e % 61 : 61 - 1 - ((-1 - e) % 61);
    x = ((x << e) & P) | x >> (61 - e);
    if (negative) x = (uint64_t)0 - x;
    return x == ~(uint64_t)0 ? x - 1 : x;  /* (-1 is -2: -1 is an error to Python) */
}

/* The 32-bit words of a whole Number's size, low first, as Python splits an int: their count. */
static size_t words_of_whole(double a, uint32_t key[33]) {
    a = fabs(a);
    if (a == 0) {
        key[0] = 0;
        return 1;
    }
    int e;
    uint64_t m = (uint64_t)ldexp(frexp(a, &e), 53);  /* a = m * 2^(e - 53), m 53 bits */
    e -= 53;
    if (e <= 0) {
        uint64_t v = m >> -e;
        key[0] = (uint32_t)v;
        key[1] = (uint32_t)(v >> 32);
        return key[1] ? 2 : 1;
    }
    size_t n = (size_t)(53 + e - 1) / 32 + 1;
    for (size_t w = 0; w < n; w++) {
        int from = 32 * (int)w - e;  /* which bit of m is the word's lowest */
        uint64_t v = from >= 64 ? 0 : from >= 0 ? m >> from : -from >= 64 ? 0 : m << -from;
        key[w] = (uint32_t)v;
    }
    return n;
}

/* A str's code points (UTF-8), or Bytes' bytes as they are (latin-1): the next one, moving *p on. */
static uint32_t next_char(const unsigned char **p, const unsigned char *end, int utf8) {
    uint32_t c = *(*p)++;
    if (!utf8 || c < 0xC0) return c;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    c &= 0x3Fu >> more;
    while (more-- && *p < end && (**p & 0xC0) == 0x80) c = c << 6 | (*(*p)++ & 0x3Fu);
    return c;
}

static void *must_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) abort();  /* LCOV_EXCL_LINE: out of memory */
    return p;
}

/* Seeds g as Python's seed(a, version) does: NULL, or why a cannot be a seed. */
static const char *seed_with(Gen *g, LubeValue a, int version) {
    int kind = lube->kind(a);
    uint32_t key[33];
    if (kind == LUBE_NOTHING) {
        seed_system(g);
        return NULL;
    }
    if (kind == LUBE_NUMBER) {
        double x = lube->number(a);
        if (isnan(x)) return "NaN cannot be a seed (it is never the same twice)";
        if (isfinite(x) && x == floor(x)) {
            init_by_array(g, key, words_of_whole(x, key));  /* as an int: its size */
        } else {
            uint64_t h = hash_double(x);  /* as a float: its hash */
            key[0] = (uint32_t)h;
            key[1] = (uint32_t)(h >> 32);
            init_by_array(g, key, key[1] ? 2 : 1);
        }
        return NULL;
    }
    if (kind != LUBE_STR && kind != LUBE_BYTES) return "a seed is a Number, a str or Bytes";
    size_t len;
    const unsigned char *s = kind == LUBE_STR ? (const unsigned char *)lube->str(a, &len) : lube->bytes(a, &len);
    if (version == 1) {  /* Python 2's way, from its characters */
        const unsigned char *p = s, *end = s + len;
        uint64_t x = 0, count = 0;
        while (p < end) {
            uint32_t c = next_char(&p, end, kind == LUBE_STR);
            if (count++ == 0) x = (uint64_t)c << 7;
            x = (UINT64_C(1000003) * x) ^ c;
        }
        x ^= count;
        key[0] = (uint32_t)x;
        key[1] = (uint32_t)(x >> 32);
        init_by_array(g, key, key[1] ? 2 : 1);
        return NULL;
    }
    /* version 2: the number whose bytes (big-endian) are the text's and then its SHA-512 */
    unsigned char digest[64];
    sha512(s, len, digest);
    size_t total = len + 64, first = 0;
    while (first < total && (first < len ? s[first] : digest[first - len]) == 0) first++;
    size_t words = total > first ? (total - first + 3) / 4 : 1;  /* (as many as the number needs: one for 0) */
    uint32_t *big = words <= 33 ? key : (uint32_t *)must_alloc(words * sizeof(uint32_t));
    for (size_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (size_t b = 0; b < 4; b++) {
            size_t from_end = 4 * w + 3 - b;  /* how far this byte is from the last */
            size_t at = total - 1 - from_end;
            v = v << 8 | (from_end < total ? (at < len ? s[at] : digest[at - len]) : 0);
        }
        big[w] = v;
    }
    init_by_array(g, big, words);
    if (big != key) free(big);
    return NULL;
}

/* ======================================================================
 * Arguments
 * ==================================================================== */

static int whole_ok(LubeValue v, double *out) {
    if (lube->kind(v) != LUBE_NUMBER) return 0;
    double x = lube->number(v);
    *out = x;
    return x == floor(x) && fabs(x) <= MAX_WHOLE;
}

/* The error for v, argument number arg, which is not a whole Number up to 2^53. */
static LubeValue not_whole(LubeVM *vm, const char *fn, LubeValue v, int arg) {
    char num[64];
    if (lube->kind(v) != LUBE_NUMBER)
        return lube->error(vm, "%s() expects a whole Number as argument %d, got %s", fn, arg, lube->type_name(v));
    lube->format_number(lube->number(v), num, sizeof num);
    if (lube->number(v) == floor(lube->number(v)))
        return lube->error(vm, "%s(): argument %d is %s, past 2^53, where whole Numbers have gaps", fn, arg, num);
    return lube->error(vm, "%s() expects a whole Number as argument %d, got %s", fn, arg, num);
}

/* What an options object (argv[i], when it is there) says for key: 1 if it says (*out borrowed). */
static int option(LubeVM *vm, int argc, const LubeValue *argv, int i, const char *key, LubeValue *out) {
    return argc > i && lube->object_get(vm, argv[i], key, out);
}

/* A population: a List, or a Range's Numbers. */
typedef struct {
    LubeValue list;
    int is_range;
    double start, step, n;
} Pop;

/* 0 if v is neither. */
static int pop_of(LubeValue v, Pop *p) {
    double end;
    int inclusive;
    p->list = v;
    p->is_range = lube->range(v, &p->start, &end, &p->step, &inclusive);
    if (!p->is_range) {
        p->n = (double)lube->list_len(v);
        return lube->kind(v) == LUBE_LIST;
    }
    double span = (end - p->start) / p->step, n;  /* (as the interpreter counts a range) */
    n = span < 0 ? 0 : inclusive ? floor(span + 1e-9) + 1 : ceil(span - 1e-9);
    p->n = n > 0 ? n : 0;
    return 1;
}

static LubeValue pop_item(const Pop *p, double i) {
    if (p->is_range) return lube->number_value(p->start + i * p->step);
    return lube->retain(lube->list_get(p->list, (size_t)i));
}

#define NEED_POP(i, pop)                                                                                       \
    do {                                                                                                       \
        NEED(pop_of(argv[i], &(pop)), i, "a List or a Range");                                                 \
        if ((pop).n > MAX_WHOLE) return lube->error(vm, "%s(): the range is too long to pick from", FNAME);    \
    } while (0)

/* Python's math.lgamma (its own, not the C library's) for whole Numbers from 1 up: binomialvariate's. */
static double lgamma_whole(double x) {
    static const double num[13] = {23531376880.410759688572007674451636754734846804940,
                                   42919803642.649098768957899047001988850926355848959,
                                   35711959237.355668049440185451547166705960488635843,
                                   17921034426.037209699919755754458931112671403265390,
                                   6039542586.3520280050642916443072979210699388420708,
                                   1439720407.3117216736632230727949123939715485786772,
                                   248874557.86205415651146038641322942321632125127801,
                                   31426415.585400194380614231628318205362874684987640,
                                   2876370.6289353724412254090516208496135991145378768,
                                   186056.26539522349504029498971604569928220784236328,
                                   8071.6720023658162106380029022722506138218516325024,
                                   210.82427775157934587250973392071336271166969580291,
                                   2.5066282746310002701649081771338373386264310793408};
    static const double den[13] = {0.0,      39916800.0, 120543840.0, 150917976.0, 105258076.0, 45995730.0, 13339535.0,
                                   2637558.0, 357423.0,  32670.0,     1925.0,      66.0,        1.0};
    static const double g = 6.024680040776729583740234375;
    if (x < 5) return x <= 2 ? 0.0 : x == 3 ? 0.693147180559945 : 1.7917594692280554;  /* (its Lanczos sum below 5) */
    double n = 0.0, d = 0.0;
    for (int i = 0; i < 13; i++) {
        n = n / x + num[i];
        d = d / x + den[i];
    }
    double r = log(n / d) - g;
    r += (x - 0.5) * (log(x + g - 0.5) - 1);
    return r;
}

/* Python's x % y for floats, y more than 0 (+ 0.0: a -0 is 0, as Python's) */
static double py_mod(double x, double y) {
    double m = fmod(x, y);
    return m < 0 ? m + y : m + 0.0;
}

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    return a % b != 0 && (a < 0) != (b < 0) ? q - 1 : q;
}

/* bisect_right(a, x, 0, hi) */
static size_t bisect_right(const double *a, double x, size_t hi) {
    size_t lo = 0;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (x < a[mid]) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

/* k distinct indexes of 0..n-1 (k <= n), drawn as Python's sample() draws them. */
static void sample_indexes(Gen *g, double n, double k, double *out) {
    double setsize = 21;
    if (k > 5) setsize += pow(4, ceil(log(k * 3) / log(4.0)));
    size_t kk = (size_t)k;
    if (n <= setsize) {  /* a pool of what is left, the last moved into each place taken */
        size_t nn = (size_t)n;
        double small[64], *pool = nn <= 64 ? small : (double *)must_alloc(nn * sizeof(double));
        for (size_t i = 0; i < nn; i++) pool[i] = (double)i;
        for (size_t i = 0; i < kk; i++) {
            size_t j = (size_t)below(g, (uint64_t)(nn - i));
            out[i] = pool[j];
            pool[j] = pool[nn - i - 1];
        }
        if (pool != small) free(pool);
        return;
    }
    size_t cap = 16;  /* or the set of those taken, drawing again when one is */
    while (cap < 2 * kk) cap *= 2;
    uint64_t *taken = (uint64_t *)must_alloc(cap * sizeof(uint64_t));  /* index + 1; 0: free */
    memset(taken, 0, cap * sizeof(uint64_t));
    for (size_t i = 0; i < kk;) {
        uint64_t j = below(g, (uint64_t)n), h = j * UINT64_C(0x9E3779B97F4A7C15);
        size_t at = (size_t)(h ^ h >> 32) & (cap - 1);
        while (taken[at] && taken[at] != j + 1) at = (at + 1) & (cap - 1);
        if (taken[at]) continue;
        taken[at] = j + 1;
        out[i++] = (double)j;
    }
    free(taken);
}

/* ======================================================================
 * The functions, each on a generator: the package's, or a Random's
 * ==================================================================== */

/* (argv: the arguments after a method's object; base 1 for a method, whose object is argument 1 in messages) */
#define RFN(name) static LubeValue r_##name(LubeVM *vm, Gen *g, int argc, const LubeValue *argv, int base)

/* seed(a?, version = 2): a whole Number seeds as Python's int, another Number as its float, a str or Bytes
 * through SHA-512 (version 1: Python 2's way); none: the system's randomness. A SystemRandom's does nothing. */
RFN(seed) {
    if (g->system) return lube->nothing();
    double version = 2;
    if (argc > 1) {
        version = lube->kind(argv[1]) == LUBE_NUMBER ? NUM(1) : 0;
        if (version != 1 && version != 2) return lube->error(vm, "seed(a, version): version is 1 or 2");
    }
    const char *why = seed_with(g, argc ? argv[0] : lube->nothing(), (int)version);
    return why ? lube->error(vm, "seed(): %s", why) : lube->nothing();
}

RFN(random) { return lube->number_value(draw(g)); }

/* getrandbits(k): k random bits, k from 0 to 53 (what a Number holds whole) */
RFN(getrandbits) {
    double k;
    WHOLE(0, k);
    if (k < 0 || k > BPF)
        return lube->error(vm, "getrandbits(k): k is from 0 to 53, the bits a Number holds (randbytes(n) gives more)");
    return lube->number_value((double)bits(g, (int)k));
}

/* randbytes(n): n random bytes, as Python's getrandbits(8 * n) in little-endian order */
RFN(randbytes) {
    double n;
    WHOLE(0, n);
    if (n < 0) return lube->error(vm, "randbytes(n): n cannot be negative");
    size_t len = (size_t)n;
    LubeValue out = lube->new_bytes(vm, NULL, len);
    unsigned char *p = lube->bytes_buffer(out);
    if (g->system) {
        system_bytes(p, len);
        return out;
    }
    for (size_t at = 0; at < len; at += 4) {
        size_t left = len - at;
        uint32_t r = next32(g);
        if (left < 4) r >>= 32 - 8 * left;
        for (size_t b = 0; b < 4 && b < left; b++) p[at + b] = (unsigned char)(r >> (8 * b));
    }
    return out;
}

/* randrange(stop), randrange(start, stop, step = 1): a whole Number of range(start, stop, step) */
RFN(randrange) {
    double a, b = 0, s = 1;
    char text[3][32];
    WHOLE(0, a);
    if (argc > 1) WHOLE(1, b);
    if (argc > 2) WHOLE(2, s);
    int64_t start = (int64_t)a, width = (int64_t)b - start, step = (int64_t)s, n;
    if (argc == 1) n = start;
    else if (step == 1) n = width;
    else if (step > 0) n = floor_div(width + step - 1, step);
    else if (step < 0) n = floor_div(width + step + 1, step);
    else return lube->error(vm, "randrange(): the step cannot be 0");
    if (n <= 0) {
        lube->format_number(a, text[0], sizeof text[0]);
        lube->format_number(b, text[1], sizeof text[1]);
        lube->format_number(s, text[2], sizeof text[2]);
        return lube->error(vm, "randrange(%s%s%s%s%s): the range is empty", text[0], argc > 1 ? ", " : "",
                           argc > 1 ? text[1] : "", argc > 2 ? ", " : "", argc > 2 ? text[2] : "");
    }
    int64_t r = (int64_t)below(g, (uint64_t)n);
    return lube->number_value((double)(argc == 1 ? r : start + step * r));
}

/* randint(a, b): a whole Number from a to b, both included */
RFN(randint) {
    double a, b;
    WHOLE(0, a);
    WHOLE(1, b);
    if (b < a) {
        char ta[32], tb[32];
        lube->format_number(a, ta, sizeof ta);
        lube->format_number(b, tb, sizeof tb);
        return lube->error(vm, "randint(%s, %s): the range is empty", ta, tb);
    }
    int64_t r = (int64_t)below(g, (uint64_t)((int64_t)b - (int64_t)a + 1));
    return lube->number_value((double)((int64_t)a + r));
}

/* choice(xs): an item of a List or a Range */
RFN(choice) {
    Pop p;
    NEED_POP(0, p);
    if (p.n == 0) return lube->error(vm, "choice(): cannot choose from an empty %s", p.is_range ? "range" : "list");
    return pop_item(&p, (double)below(g, (uint64_t)p.n));
}

/* shuffle(xs): a new list of xs's items in a random order: the order Python's shuffle(xs) leaves xs in (a
 * plugin cannot change a list in place) */
RFN(shuffle) {
    NEED(lube->kind(argv[0]) == LUBE_LIST, 0, "a List");
    size_t n = lube->list_len(argv[0]), small[64];
    size_t *order = n <= 64 ? small : (size_t *)must_alloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = n; i-- > 1;) {
        size_t j = (size_t)below(g, (uint64_t)i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    LubeValue out = lube->new_list(vm);
    for (size_t i = 0; i < n; i++) lube->list_push(out, lube->retain(lube->list_get(argv[0], order[i])));
    if (order != small) free(order);
    return out;
}

/* sample(xs, k, {counts}): k items of a List or a Range, none twice, in the order picked; counts: how many
 * times each item is there */
RFN(sample) {
    Pop p;
    double k, total, small[64], *cum = small, *picked = small;
    LubeValue counts, out;
    NEED_POP(0, p);
    WHOLE(1, k);
    if (argc > 2) NEED(lube->kind(argv[2]) == LUBE_OBJECT, 2, "an object of options");
    int with_counts = option(vm, argc, argv, 2, "counts", &counts);
    size_t m = 0;
    if (with_counts) {
        if (lube->kind(counts) != LUBE_LIST) return lube->error(vm, "sample(): counts is a List of whole Numbers");
        m = lube->list_len(counts);
        if ((double)m != p.n) return lube->error(vm, "sample(): the number of counts does not match the population");
        if (m > 64) cum = (double *)must_alloc(m * sizeof(double));
        for (size_t i = 0; i < m; i++) {
            double c;
            if (!whole_ok(lube->list_get(counts, i), &c)) {
                if (cum != small) free(cum);
                return lube->error(vm, "sample(): counts must be whole Numbers");
            }
            cum[i] = i ? cum[i - 1] + c : c;
        }
        total = m ? cum[m - 1] : 0;
        if (total < 0) {
            if (cum != small) free(cum);
            return lube->error(vm, "sample(): counts cannot add up to less than 0");
        }
    } else {
        total = p.n;
        cum = NULL;
    }
    if (!(k >= 0 && k <= total)) {
        if (cum && cum != small) free(cum);
        return lube->error(vm, "sample(): k must be from 0 to the size of the population");
    }
    if (with_counts || k > 64) picked = (double *)must_alloc((size_t)k * sizeof(double));  /* (small holds the counts) */
    sample_indexes(g, total, k, picked);
    out = lube->new_list(vm);
    for (size_t i = 0; i < (size_t)k; i++)
        lube->list_push(out, pop_item(&p, cum ? (double)bisect_right(cum, picked[i], m - 1) : picked[i]));
    if (picked != small) free(picked);
    if (cum && cum != small) free(cum);
    return out;
}

/* choices(xs, weights?, {weights, cum_weights, k = 1}): k items of a List or a Range, picked with repeats; by
 * weight, or by cumulative weights */
RFN(choices) {
    Pop p;
    LubeValue weights = lube->nothing(), v;
    double k = 1, total, small[64], *cum = small;
    int opts = 1, cumulative = 0;
    NEED_POP(0, p);
    if (argc > 1 && lube->kind(argv[1]) == LUBE_LIST) {
        weights = argv[1];
        opts = 2;
    }
    if (argc > opts + 1) NEED(0, 1, "a List of weights");  /* (three arguments: the weights, then the options) */
    if (argc > opts)
        NEED(lube->kind(argv[opts]) == LUBE_OBJECT, opts, opts == 1 ? "a List of weights or an object of options" : "an object of options");
    if (option(vm, argc, argv, opts, "weights", &v)) {
        if (lube->kind(weights) != LUBE_NOTHING) return lube->error(vm, "choices(): the weights are given twice");
        weights = v;
    }
    if (option(vm, argc, argv, opts, "cum_weights", &v)) {
        if (lube->kind(weights) != LUBE_NOTHING) return lube->error(vm, "choices(): give weights or cum_weights, not both");
        weights = v;
        cumulative = 1;
    }
    if (option(vm, argc, argv, opts, "k", &v) && !whole_ok(v, &k)) return lube->error(vm, "choices(): k is a whole Number");
    LubeValue out = lube->new_list(vm);
    if (lube->kind(weights) == LUBE_NOTHING) {  /* all alike */
        for (double i = 0; i < k; i++) {
            size_t at = (size_t)floor(draw(g) * p.n);
            if (p.n == 0) {
                lube->release(out);
                return lube->error(vm, "choices(): cannot choose from an empty %s", p.is_range ? "range" : "list");
            }
            lube->list_push(out, pop_item(&p, (double)at));
        }
        return out;
    }
    size_t n = lube->list_len(weights);
    const char *bad = lube->kind(weights) != LUBE_LIST ? "the weights are a List of Numbers"
                    : (double)n != p.n ? "the number of weights does not match the population"
                    : n == 0 ? "cannot choose from an empty list" : NULL;
    if (!bad && n > 64) cum = (double *)must_alloc(n * sizeof(double));
    for (size_t i = 0; !bad && i < n; i++) {
        LubeValue w = lube->list_get(weights, i);
        if (lube->kind(w) != LUBE_NUMBER) bad = "the weights are a List of Numbers";
        cum[i] = cumulative || !i ? lube->number(w) : cum[i - 1] + lube->number(w);
    }
    total = bad ? 0 : cum[n - 1] + 0.0;
    if (!bad && total <= 0.0) bad = "the weights must add up to more than 0";
    if (!bad && !isfinite(total)) bad = "the weights must add up to a finite Number";
    for (double i = 0; !bad && i < k; i++)
        lube->list_push(out, pop_item(&p, (double)bisect_right(cum, draw(g) * total, n - 1)));
    if (cum != small) free(cum);
    if (!bad) return out;
    lube->release(out);
    return lube->error(vm, "choices(): %s", bad);
}

RFN(uniform) {
    NEED_NUM(0);
    NEED_NUM(1);
    double a = NUM(0), b = NUM(1);
    return lube->number_value(a + (b - a) * draw(g));
}

/* triangular(low = 0, high = 1, mode = halfway) */
RFN(triangular) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    double low = NUM_OR(0, 0.0), high = NUM_OR(1, 1.0), u = draw(g), c = 0.5;
    if (argc > 2) {
        if (high - low == 0) return lube->number_value(low);
        c = (NUM(2) - low) / (high - low);
    }
    if (u > c) {
        double t = low;
        u = 1.0 - u;
        c = 1.0 - c;
        low = high;
        high = t;
    }
    return lube->number_value(low + (high - low) * sqrt(u * c));
}

static double normal(Gen *g, double mu, double sigma) {
    double z = 0;
    for (;;) {
        double u1 = draw(g), u2 = 1.0 - draw(g);
        z = NV_MAGICCONST * (u1 - 0.5) / u2;
        if (z * z / 4.0 <= -log(u2)) break;
    }
    return mu + z * sigma;
}

/* normalvariate(mu = 0, sigma = 1) */
RFN(normalvariate) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    return lube->number_value(normal(g, NUM_OR(0, 0.0), NUM_OR(1, 1.0)));
}

/* gauss(mu = 0, sigma = 1): makes two numbers at a time, and gives the second next time */
RFN(gauss) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    double z;
    if (g->has_gauss) {
        z = g->gauss_next;
        g->has_gauss = 0;
    } else {
        double x2pi = draw(g) * TWOPI;
        double g2rad = sqrt(-2.0 * log(1.0 - draw(g)));
        z = cos(x2pi) * g2rad;
        g->gauss_next = sin(x2pi) * g2rad;
        g->has_gauss = 1;
    }
    return lube->number_value(NUM_OR(0, 0.0) + z * NUM_OR(1, 1.0));
}

RFN(lognormvariate) {
    NEED_NUM(0);
    NEED_NUM(1);
    return lube->number_value(exp(normal(g, NUM(0), NUM(1))));
}

/* expovariate(lambd = 1): lambd is 1 / the mean */
RFN(expovariate) {
    if (argc) NEED_NUM(0);
    double lambd = NUM_OR(0, 1.0), x = -log(1.0 - draw(g));
    if (lambd == 0) return lube->error(vm, "expovariate(): lambd cannot be 0 (it is 1 / the mean)");
    return lube->number_value(x / lambd);
}

/* vonmisesvariate(mu, kappa): an angle from 0 to 2 pi */
RFN(vonmisesvariate) {
    NEED_NUM(0);
    NEED_NUM(1);
    double mu = NUM(0), kappa = NUM(1), z = 0;
    if (isnan(kappa)) return lube->error(vm, "vonmisesvariate(): kappa cannot be NaN (Python's never returns)");
    if (kappa <= 1e-6) return lube->number_value(TWOPI * draw(g));
    double s = 0.5 / kappa, r = s + sqrt(1.0 + s * s);
    for (;;) {
        double u1 = draw(g);
        z = cos(PI * u1);
        double d = z / (r + z), u2 = draw(g);
        if (u2 < 1.0 - d * d || u2 <= (1.0 - d) * exp(d)) break;
    }
    double q = 1.0 / r, f = (q + z) / (1.0 + q * z), u3 = draw(g);
    return lube->number_value(py_mod(u3 > 0.5 ? mu + acos(f) : mu - acos(f), TWOPI));
}

/* What is wrong with gammavariate's alpha and beta, or NULL. */
static const char *gamma_bad(double alpha, double beta) {
    if (alpha <= 0.0 || beta <= 0.0) return "alpha and beta must be more than 0";
    if (!isfinite(alpha)) return "alpha must be finite (Python's never returns)";
    return NULL;
}

static double gamma_draw(Gen *g, double alpha, double beta) {
    if (alpha > 1.0) {  /* R.C.H. Cheng's method */
        double ainv = sqrt(2.0 * alpha - 1.0), bbb = alpha - LOG4, ccc = alpha + ainv;
        for (;;) {
            double u1 = draw(g);
            if (!(1e-7 < u1 && u1 < 0.9999999)) continue;
            double u2 = 1.0 - draw(g);
            double v = log(u1 / (1.0 - u1)) / ainv, x = alpha * exp(v), z = u1 * u1 * u2, r = bbb + ccc * v - x;
            if (r + SG_MAGICCONST - 4.5 * z >= 0.0 || r >= log(z)) return x * beta;
        }
    }
    if (alpha == 1.0) return -log(1.0 - draw(g)) * beta;
    double x = 0;
    for (;;) {  /* alpha below 1: algorithm GS of Kennedy and Gentle */
        double u = draw(g), b = (EULER + alpha) / EULER, p = b * u;
        x = p <= 1.0 ? pow(p, 1.0 / alpha) : -log((b - p) / alpha);
        double u1 = draw(g);
        if (p > 1.0 ? u1 <= pow(x, alpha - 1.0) : u1 <= exp(-x)) break;
    }
    return x * beta;
}

/* gammavariate(alpha, beta): not the gamma function */
RFN(gammavariate) {
    NEED_NUM(0);
    NEED_NUM(1);
    const char *bad = gamma_bad(NUM(0), NUM(1));
    if (bad) return lube->error(vm, "gammavariate(): %s", bad);
    return lube->number_value(gamma_draw(g, NUM(0), NUM(1)));
}

/* betavariate(alpha, beta): from 0 to 1 */
RFN(betavariate) {
    NEED_NUM(0);
    NEED_NUM(1);
    const char *bad = gamma_bad(NUM(0), 1.0);
    if (!bad) bad = gamma_bad(NUM(1), 1.0);
    if (bad) return lube->error(vm, "betavariate(): %s", bad);
    double y = gamma_draw(g, NUM(0), 1.0);
    return lube->number_value(y ? y / (y + gamma_draw(g, NUM(1), 1.0)) : 0.0);
}

RFN(paretovariate) {
    NEED_NUM(0);
    double u = 1.0 - draw(g);
    if (NUM(0) == 0) return lube->error(vm, "paretovariate(): alpha cannot be 0");
    return lube->number_value(pow(u, -1.0 / NUM(0)));
}

/* weibullvariate(alpha, beta): alpha the scale, beta the shape */
RFN(weibullvariate) {
    NEED_NUM(0);
    NEED_NUM(1);
    double u = 1.0 - draw(g);
    if (NUM(1) == 0) return lube->error(vm, "weibullvariate(): beta cannot be 0");
    return lube->number_value(NUM(0) * pow(-log(u), 1.0 / NUM(1)));
}

static double binomial(Gen *g, double n, double p) {
    if (n == 1) return draw(g) < p ? 1 : 0;
    if (p > 0.5) return n - binomial(g, n, 1.0 - p);
    if (n * p < 10.0) {  /* Devroye's geometric method */
        double x = 0, y = 0, c = log2(1.0 - p);
        if (!c) return x;
        for (;;) {
            double u = draw(g);
            if (u == 0) continue;  /* (log2(0) is an error to Python, which draws again) */
            y += floor(log2(u) / c) + 1;
            if (y > n) return x;
            x += 1;
        }
    }
    /* BTRS: Hormann's transformed rejection with squeeze */
    double spq = sqrt(n * p * (1.0 - p)), b = 1.15 + 2.53 * spq, a = -0.0873 + 0.0248 * b + 0.01 * p,
           c = n * p + 0.5, vr = 0.92 - 4.2 / b, alpha = 0, lpq = 0, m = 0, h = 0;
    int ready = 0;
    for (;;) {
        double u = draw(g) - 0.5, us = 0.5 - fabs(u);
        if (us == 0) continue;  /* (a division by zero to Python, which draws again) */
        double k = floor((2.0 * a / us + b) * u + c);
        if (k < 0 || k > n) continue;
        double v = draw(g);
        if (us >= 0.07 && v <= vr) return k;
        if (!ready) {
            alpha = (2.83 + 5.1 / b) * spq;
            lpq = log(p / (1.0 - p));
            m = floor((n + 1) * p);
            h = lgamma_whole(m + 1) + lgamma_whole(n - m + 1);
            ready = 1;
        }
        v *= alpha / (a / (us * us) + b);
        if (log(v) <= h - lgamma_whole(k + 1) - lgamma_whole(n - k + 1) + (k - m) * lpq) return k;
    }
}

/* binomialvariate(n = 1, p = 0.5): how many of n tries succeed, each with the chance p */
RFN(binomialvariate) {
    double n = 1, p = 0.5;
    if (argc > 0) WHOLE(0, n);
    if (argc > 1) {
        NEED_NUM(1);
        p = NUM(1);
    }
    if (n < 0) return lube->error(vm, "binomialvariate(): n cannot be negative");
    if (!(p >= 0 && p <= 1)) return lube->error(vm, "binomialvariate(): p is from 0 to 1");
    return lube->number_value(p == 0 ? 0 : p == 1 ? n : binomial(g, n, p));
}

/* getstate(): [3, [the 624 words, where the next one is], what gauss() has waiting or false] */
RFN(getstate) {
    if (g->system) return lube->error(vm, "getstate(): a SystemRandom has no state");
    LubeValue words = lube->new_list(vm), state = lube->new_list(vm);
    for (int i = 0; i < MT_N; i++) lube->list_push(words, lube->number_value(g->mt[i]));
    lube->list_push(words, lube->number_value(g->index));
    lube->list_push(state, lube->number_value(3));
    lube->list_push(state, words);
    lube->list_push(state, g->has_gauss ? lube->number_value(g->gauss_next) : lube->boolean(0));
    return state;
}

/* setstate(state): back to what getstate() gave (or Python's, as a list); version 2's words may be negative */
RFN(setstate) {
    if (g->system) return lube->error(vm, "setstate(): a SystemRandom has no state");
    LubeValue s = argv[0], words = lube->list_get(s, 1), gauss = lube->list_get(s, 2);
    double version = lube->number(lube->list_get(s, 0));
    if (lube->kind(s) != LUBE_LIST || lube->list_len(s) != 3 || lube->kind(lube->list_get(s, 0)) != LUBE_NUMBER ||
        lube->kind(words) != LUBE_LIST ||
        lube->list_len(words) != MT_N + 1 || (lube->kind(gauss) != LUBE_NUMBER && lube->kind(gauss) != LUBE_BOOL))
        return lube->error(vm, "setstate(): that is not a state from getstate()");
    if (version != 2 && version != 3) {
        char text[32];
        lube->format_number(version, text, sizeof text);
        return lube->error(vm, "setstate(): a state of version %s, where Random's is 3", text);
    }
    uint32_t mt[MT_N + 1];
    for (int i = 0; i <= MT_N; i++) {
        double w;
        if (!whole_ok(lube->list_get(words, (size_t)i), &w))
            return lube->error(vm, "setstate(): the state's words are whole Numbers");
        if (version == 2) w = fmod(fmod(w, 4294967296.0) + 4294967296.0, 4294967296.0);  /* (x % 2**32) */
        if (w < 0 || w > 4294967295.0 || (i == MT_N && w > MT_N))
            return lube->error(vm, "setstate(): the state's words are from 0 to 4294967295, and its place from 0 to 624");
        mt[i] = (uint32_t)w;
    }
    memcpy(g->mt, mt, sizeof g->mt);
    g->index = (int)mt[MT_N];
    g->has_gauss = lube->kind(gauss) == LUBE_NUMBER;
    g->gauss_next = lube->number(gauss);
    return lube->nothing();
}

/* ======================================================================
 * The package's functions, Random and SystemRandom
 * ==================================================================== */

static Gen *gen_of(LubeValue v) {
    Gen *g = (Gen *)lube->foreign(v, RandomType);
    return g ? g : (Gen *)lube->foreign(v, SystemType);
}

/* random.name(...) on the package's generator (under the lock), and r.name(...) on r's */
#define BOTH(name)                                                                                             \
    FN(f_##name) {                                                                                             \
        lube->lock_acquire(lock);                                                                              \
        LubeValue r = r_##name(vm, &shared_gen, argc, argv, 0);                                                \
        lube->lock_release(lock);                                                                              \
        return r;                                                                                              \
    }                                                                                                          \
    FN(m_##name) { return r_##name(vm, gen_of(argv[0]), argc - 1, argv + 1, 1); }

BOTH(seed)
BOTH(random)
BOTH(getrandbits)
BOTH(randbytes)
BOTH(randrange)
BOTH(randint)
BOTH(choice)
BOTH(shuffle)
BOTH(sample)
BOTH(choices)
BOTH(uniform)
BOTH(triangular)
BOTH(normalvariate)
BOTH(gauss)
BOTH(lognormvariate)
BOTH(expovariate)
BOTH(vonmisesvariate)
BOTH(gammavariate)
BOTH(betavariate)
BOTH(paretovariate)
BOTH(weibullvariate)
BOTH(binomialvariate)
BOTH(getstate)
BOTH(setstate)

#define BOTH_ENTRY(name, min, max) {#name, f_##name, m_##name, min, max}
static const struct {
    const char *name;
    LubeFn fn, method;
    int min, max;  /* (not counting a method's object) */
} FUNCTIONS[] = {
    BOTH_ENTRY(seed, 0, 2),           BOTH_ENTRY(random, 0, 0),         BOTH_ENTRY(getrandbits, 1, 1),
    BOTH_ENTRY(randbytes, 1, 1),      BOTH_ENTRY(randrange, 1, 3),      BOTH_ENTRY(randint, 2, 2),
    BOTH_ENTRY(choice, 1, 1),         BOTH_ENTRY(shuffle, 1, 1),        BOTH_ENTRY(sample, 2, 3),
    BOTH_ENTRY(choices, 1, 3),        BOTH_ENTRY(uniform, 2, 2),        BOTH_ENTRY(triangular, 0, 3),
    BOTH_ENTRY(normalvariate, 0, 2),  BOTH_ENTRY(gauss, 0, 2),          BOTH_ENTRY(lognormvariate, 2, 2),
    BOTH_ENTRY(expovariate, 0, 1),    BOTH_ENTRY(vonmisesvariate, 2, 2), BOTH_ENTRY(gammavariate, 2, 2),
    BOTH_ENTRY(betavariate, 2, 2),    BOTH_ENTRY(paretovariate, 1, 1),  BOTH_ENTRY(weibullvariate, 2, 2),
    BOTH_ENTRY(binomialvariate, 0, 2), BOTH_ENTRY(getstate, 0, 0),      BOTH_ENTRY(setstate, 1, 1),
};

/* Random(seed?): a generator of its own, seeded as seed(a) seeds (none: from the system's randomness) */
FN(f_Random) {
    void *data;
    LubeValue r = lube->new_foreign(vm, RandomType, &data);
    const char *why = seed_with((Gen *)data, argc ? argv[0] : lube->nothing(), 2);
    if (!why) return r;
    lube->release(r);
    return lube->error(vm, "Random(): %s", why);
}

/* SystemRandom(seed?): the system's random bytes (a seed is let go of, as in Python) */
FN(f_SystemRandom) {
    void *data;
    LubeValue r = lube->new_foreign(vm, SystemType, &data);
    ((Gen *)data)->system = 1;
    return r;
}

static int describe_random(const void *data, char *buf, size_t size) {
    return snprintf(buf, size, ((const Gen *)data)->system ? "SystemRandom()" : "Random()");
}

/* What this plugin gives others (lube->require(plugin, "random")): its generators. statistics.c has a copy. */
typedef struct {
    int version;                                 /* 1 */
    size_t size;                                 /* a generator's bytes */
    const char *(*seed)(void *gen, LubeValue a); /* seeds it as Random(a) does: NULL, or why a cannot be a seed */
    double (*random)(void *gen);                 /* random() */
    double (*below)(void *gen, double n);        /* a whole Number from 0 to n - 1, as choice() picks (n up to 2^53) */
    void *(*shared)(int take);                   /* 1: the package's generator, locked; 0: unlocks it */
} RandomGives;

static const char *give_seed(void *gen, LubeValue a) {
    memset(gen, 0, sizeof(Gen));
    return seed_with((Gen *)gen, a, 2);
}

static double give_random(void *gen) { return draw((Gen *)gen); }
static double give_below(void *gen, double n) { return (double)below((Gen *)gen, (uint64_t)n); }

static void *give_shared(int take) {
    if (take) {
        lube->lock_acquire(lock);
        return &shared_gen;
    }
    lube->lock_release(lock);
    return NULL;
}

static const RandomGives gives = {1, sizeof(Gen), give_seed, give_random, give_below, give_shared};

LUBE_PLUGIN(random) {
    lube = api;
    if (api->version < 2) return -1;  /* LCOV_EXCL_LINE: (an older Lube) */
    if (!lock) {  /* (once: plugins load one at a time) */
        lock = api->lock_new();
        seed_system(&shared_gen);  /* as Python's: from the system's randomness */
    }
    api->provide(plugin, &gives);
    RandomType = api->type(plugin, "Random", sizeof(Gen), NULL, describe_random);
    api->constructor(RandomType, f_Random, 0, 1);
    SystemType = api->type(plugin, "SystemRandom", sizeof(Gen), NULL, describe_random);
    api->constructor(SystemType, f_SystemRandom, 0, 1);
    for (size_t i = 0; i < sizeof FUNCTIONS / sizeof FUNCTIONS[0]; i++) {
        api->function(plugin, FUNCTIONS[i].name, FUNCTIONS[i].fn, FUNCTIONS[i].min, FUNCTIONS[i].max);
        api->method(RandomType, FUNCTIONS[i].name, FUNCTIONS[i].method, FUNCTIONS[i].min + 1, FUNCTIONS[i].max + 1);
        api->method(SystemType, FUNCTIONS[i].name, FUNCTIONS[i].method, FUNCTIONS[i].min + 1, FUNCTIONS[i].max + 1);
    }
    api->constant(plugin, "BPF", BPF);
    api->constant(plugin, "RECIP_BPF", RECIP_BPF);
    api->constant(plugin, "LOG4", LOG4);
    api->constant(plugin, "NV_MAGICCONST", NV_MAGICCONST);
    api->constant(plugin, "SG_MAGICCONST", SG_MAGICCONST);
    api->constant(plugin, "TWOPI", TWOPI);
    return LUBE_PLUGIN_API;
}
