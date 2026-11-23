/*
 * web/util.c - the web plugin's small tools: growing buffers, the system's
 * random bytes, SHA-1 and base64 (WebSocket handshakes), SipHash (handler
 * ids), UTF-8 checks and a strict JSON reader for the browser's messages.
 * (Part of web.c.)
 */

/* ---- a growing buffer of bytes ---- */

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void buf_need(Buf *b, size_t more) {
    if (b->n + more <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more) cap *= 2;
    char *p = (char *)realloc(b->p, cap);
    if (!p) abort();  /* LCOV_EXCL_LINE: out of memory */
    b->p = p;
    b->cap = cap;
}

static void buf_add(Buf *b, const void *p, size_t n) {
    if (!n) return;
    buf_need(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_fmt(Buf *b, const char *format, ...) {
    va_list ap, again;
    va_start(ap, format);
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (n > 0) {
        buf_need(b, (size_t)n + 1);
        vsnprintf(b->p + b->n, (size_t)n + 1, format, again);
        b->n += (size_t)n;
    }
    va_end(again);
}

/* Drops the first n bytes. */
static void buf_drop(Buf *b, size_t n) {
    if (n >= b->n) {
        b->n = 0;
        return;
    }
    memmove(b->p, b->p + n, b->n - n);
    b->n -= n;
}

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* Text, escaped for HTML: in text and in attribute values alike. */
static void buf_escape(Buf *b, const char *s, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *e = NULL;
        switch (s[i]) {
        case '&': e = "&amp;"; break;
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '"': e = "&quot;"; break;
        case '\'': e = "&#39;"; break;
        default: continue;
        }
        buf_add(b, s + start, i - start);
        buf_str(b, e);
        start = i + 1;
    }
    buf_add(b, s + start, n - start);
}

/* Text as a JSON string, quotes and all. */
static void buf_json(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    buf_add(b, "\"", 1);
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\' && c != '<' && c != '>' && c != '&') continue;
        buf_add(b, s + start, i - start);
        char e[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
        if (c == '"' || c == '\\') {
            e[1] = (char)c;
            e[2] = 0;
        } else if (c == '\n') {
            e[1] = 'n';
            e[2] = 0;
        }
        buf_str(b, e);
        start = i + 1;
    }
    buf_add(b, s + start, n - start);
    buf_add(b, "\"", 1);
}

/* ---- the system's random bytes (0 if it has none: the plugin then refuses to serve) ---- */

static int system_random(void *buf, size_t n) {
#if defined(_WIN32)
    unsigned char *p = (unsigned char *)buf;
    while (n) {
        unsigned int v;
        if (rand_s(&v)) return 0;  /* LCOV_EXCL_LINE */
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
    if (!f) return 0;  /* LCOV_EXCL_LINE */
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

/* n random bytes as 2n hex digits (and a NUL): 0 if the system has no random bytes. */
static int random_hex(char *out, size_t n) {
    unsigned char raw[64];
    static const char hex[] = "0123456789abcdef";
    if (n > sizeof raw || !system_random(raw, n)) return 0;  /* LCOV_EXCL_LINE */
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[2 * n] = 0;
    return 1;
}

/* ---- SHA-1 (only for Sec-WebSocket-Accept, which the protocol defines with it) ---- */

static uint32_t rol32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static void sha1_block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol32(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

/* SHA-1 of n bytes, n < 120 (a WebSocket key and the protocol's GUID are 60): two blocks at most. */
static void sha1(const void *data, size_t n, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    unsigned char last[128] = {0};
    memcpy(last, data, n);
    last[n] = 0x80;
    size_t blocks = n + 9 <= 64 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
    for (size_t i = 0; i < blocks; i++) sha1_block(h, last + 64 * i);
    for (int i = 0; i < 20; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static void base64(const unsigned char *p, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---- SipHash-2-4: a page's handler ids, from its secret key ---- */

static uint64_t rol64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

#define SIPROUND                                                     \
    do {                                                             \
        v0 += v1, v1 = rol64(v1, 13), v1 ^= v0, v0 = rol64(v0, 32);  \
        v2 += v3, v3 = rol64(v3, 16), v3 ^= v2;                      \
        v0 += v3, v3 = rol64(v3, 21), v3 ^= v0;                      \
        v2 += v1, v1 = rol64(v1, 17), v1 ^= v2, v2 = rol64(v2, 32);  \
    } while (0)

/* SipHash-2-4 of one 64-bit word m under key k. */
static uint64_t siphash(const uint64_t k[2], uint64_t m) {
    uint64_t v0 = k[0] ^ 0x736f6d6570736575ull, v1 = k[1] ^ 0x646f72616e646f6dull;
    uint64_t v2 = k[0] ^ 0x6c7967656e657261ull, v3 = k[1] ^ 0x7465646279746573ull;
    uint64_t b = (uint64_t)8 << 56;
    v3 ^= m;
    SIPROUND;
    SIPROUND;
    v0 ^= m;
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/* ---- UTF-8 ---- */

/* Whether s is well-formed UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF). */
static int utf8_ok(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int more;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) more = 1;
        else if (c >= 0xE0 && c <= 0xEF) more = 2, lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
        else if (c >= 0xF0 && c <= 0xF4) more = 3, lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
        else return 0;
        if (i + (size_t)more >= n) return 0;
        if (p[i + 1] < lo || p[i + 1] > hi) return 0;
        for (int k = 2; k <= more; k++)
            if (p[i + (size_t)k] < 0x80 || p[i + (size_t)k] > 0xBF) return 0;
        i += (size_t)more + 1;
    }
    return 1;
}

/* ---- JSON, strictly: what the browser sends (an event), nothing more ----
 * A value is a string, true, false, a number, or an object or list of those, nested at most 4 deep. */

typedef struct {
    const char *p, *end;
    LubeVM *vm;
    int depth;
} Json;

static void json_space(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_put(Buf *b, uint32_t c) {
    char u[4];
    size_t n;
    if (c < 0x80) u[0] = (char)c, n = 1;
    else if (c < 0x800) u[0] = (char)(0xC0 | c >> 6), u[1] = (char)(0x80 | (c & 63)), n = 2;
    else if (c < 0x10000)
        u[0] = (char)(0xE0 | c >> 12), u[1] = (char)(0x80 | (c >> 6 & 63)), u[2] = (char)(0x80 | (c & 63)), n = 3;
    else
        u[0] = (char)(0xF0 | c >> 18), u[1] = (char)(0x80 | (c >> 12 & 63)), u[2] = (char)(0x80 | (c >> 6 & 63)),
        u[3] = (char)(0x80 | (c & 63)), n = 4;
    buf_add(b, u, n);
}

/* A string's text (after its opening quote) into b: 0 if it is not well formed. */
static int json_text(Json *j, Buf *b) {
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c != '\\') {
            buf_add(b, &c, 1);
            continue;
        }
        if (j->p >= j->end) return 0;
        char e = *j->p++;
        const char *plain = strchr("\"\\/bfnrt", e);
        if (e && plain) {
            static const char to[] = "\"\\/\b\f\n\r\t";
            buf_add(b, &to[plain - "\"\\/bfnrt"], 1);
            continue;
        }
        if (e != 'u' || j->end - j->p < 4) return 0;
        uint32_t u = 0;
        for (int i = 0; i < 4; i++) {
            int d = hex_digit(*j->p++);
            if (d < 0) return 0;
            u = u << 4 | (uint32_t)d;
        }
        if (u >= 0xD800 && u <= 0xDBFF) {  /* a pair */
            if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') return 0;
            j->p += 2;
            uint32_t lo = 0;
            for (int i = 0; i < 4; i++) {
                int d = hex_digit(*j->p++);
                if (d < 0) return 0;
                lo = lo << 4 | (uint32_t)d;
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return 0;
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return 0;
        }
        utf8_put(b, u);
    }
    return 0;
}

/* One value into *out (a new value): 0 if it is not well formed. */
static int json_value(Json *j, LubeValue *out) {
    json_space(j);
    if (j->p >= j->end) return 0;
    char c = *j->p;
    if (c == '"') {
        j->p++;
        Buf b = {0};
        int ok = json_text(j, &b) && utf8_ok(b.p, b.n);
        if (ok) *out = lube->new_str(j->vm, b.n ? b.p : "", b.n);
        buf_free(&b);
        return ok;
    }
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) {
        j->p += 4;
        *out = lube->boolean(1);
        return 1;
    }
    if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) {
        j->p += 5;
        *out = lube->boolean(0);
        return 1;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char num[64];
        size_t n = 0;
        while (j->p < j->end && n < sizeof num - 1 && strchr("+-.eE0123456789", *j->p) && *j->p) num[n++] = *j->p++;
        num[n] = 0;
        char *stop;
        double x = strtod(num, &stop);
        if (*stop || !n || x != x) return 0;
        *out = lube->number_value(x);
        return 1;
    }
    if ((c != '{' && c != '[') || j->depth >= 4) return 0;
    j->p++;
    j->depth++;
    LubeValue all = c == '{' ? lube->new_object(j->vm) : lube->new_list(j->vm);
    json_space(j);
    char close = c == '{' ? '}' : ']';
    int ok = 1;
    if (j->p < j->end && *j->p == close) {
        j->p++;
    } else {
        for (;;) {
            Buf key = {0};
            if (c == '{') {
                json_space(j);
                ok = j->p < j->end && *j->p++ == '"' && json_text(j, &key) && utf8_ok(key.p, key.n) &&
                     !memchr(key.p ? key.p : "", 0, key.n);
                if (ok) {
                    json_space(j);
                    ok = j->p < j->end && *j->p++ == ':';
                }
            }
            LubeValue item = lube->nothing();
            if (ok) ok = json_value(j, &item);
            if (ok && c == '{') {
                buf_add(&key, "", 1);
                lube->object_set(j->vm, all, key.p, item);
            } else if (ok) {
                lube->list_push(all, item);
            }
            buf_free(&key);
            if (!ok) break;
            json_space(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            ok = j->p < j->end && *j->p++ == close;
            break;
        }
    }
    j->depth--;
    if (!ok) {
        lube->release(all);
        return 0;
    }
    *out = all;
    return 1;
}
