/*
 * io.c - bytes and time: what games, their tools and their plugins share.
 *
 *   Bytes   a buffer of bytes, changeable in place (copied when shared, as
 *           a list is): Bytes(n), Bytes("text"), Bytes([1, 2]); b[i], len,
 *           push, +, for x in b; and typed reads and writes for binary
 *           formats and network packets: b.pack("u16", 5), b.unpack("f32", 8).
 *   time    sleep(seconds), and now(): a steady clock for timing frames.
 *
 * Files, sockets and sound are plugins (plugins/fs.c, net.c, audio.c).
 * Bytes' methods are natives in a table of their own (LUBE_METHOD), found by
 * x.name(...) before a global fn of that name (interp.c).
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* nanosleep, clock_gettime under -std=c99 */
#endif
#include "lube.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define TokenType WinTokenType  /* winnt.h has a TokenType of its own */
#include <windows.h>
#undef TokenType
#endif

#define NATIVE(name) static Value name(Interp *I, int argc, Value *argv)
#define FNAME (__func__ + 2)  /* "n_open" -> "open" */
#define NEED(cond, i, what) \
    do { if (!(cond)) return lube_error(I, "%s() expects %s as argument %d, got %s", FNAME, (what), (i) + 1, type_name(argv[i])); } while (0)
#define NEED_STR(i) NEED(IS_TEXT(argv[i]), i, "a str")
#define NEED_NUM(i) NEED(IS_NUMERIC(argv[i]), i, "a Number")

/* ======================================================================
 * Bytes
 * ==================================================================== */

ObjBytes *new_bytes(size_t cap) {
    ObjBytes *b = (ObjBytes *)alloc_object(sizeof(ObjBytes), OBJ_BYTES);
    b->len = 0;
    b->cap = cap ? cap : 0;
    b->data = cap ? lube_block_alloc(cap) : NULL;
    return b;
}

void bytes_reserve(ObjBytes *b, size_t n) {
    if (n <= b->cap) return;
    if (n > LUBE_MAX_BYTES) lube_out_of_memory();
    size_t cap = b->cap ? b->cap * 2 : 16;
    while (cap < n) cap *= 2;
    b->data = lube_block_realloc(b->data, b->cap, cap);
    b->cap = cap;
}

void bytes_add(ObjBytes *b, const void *p, size_t n) {
    bytes_reserve(b, b->len + n);
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* The error for a value that is not a byte. */
static Value not_a_byte(Interp *I, const char *fn, Value v) {
    if (IS_NUMERIC(v)) {
        char buf[64];
        format_number(buf, sizeof buf, numeric_value(v));
        return lube_error(I, "%s%s is not a byte (a whole Number from 0 to 255)", fn, buf);
    }
    return lube_error(I, "%sa byte is a whole Number from 0 to 255, not %s", fn, a_type(v));
}

/* A byte: a whole Number from 0 to 255 (or a sized integer in range). 0 if not. */
static int byte_of(Value v, unsigned char *out) {
    if (!IS_NUMERIC(v)) return 0;
    double d = numeric_value(v);
    if (d != floor(d) || d < 0 || d > 255) return 0;
    *out = (unsigned char)d;
    return 1;
}

/* Bytes(), Bytes(n): n zero bytes, Bytes("text"), Bytes([1, 2, 3]), Bytes(bytes). */
NATIVE(n_Bytes) {
    if (argc == 0) return OBJ_VAL(new_bytes(0));
    Value v = argv[0];
    if (IS_BYTES(v)) return retain(v);  /* (a copy: shared until one changes) */
    if (IS_TEXT(v)) {
        StrTemp s_t;
        ObjString *s = text_in(v, &s_t);
        ObjBytes *b = new_bytes(s->length);
        bytes_add(b, s->chars, s->length);
        return OBJ_VAL(b);
    }
    if (IS_NUMERIC(v)) {
        double d = numeric_value(v);
        if (d != floor(d) || d < 0) return lube_error(I, "Bytes(n): n must be a whole Number, 0 or more");
        if (d > (double)LUBE_MAX_BYTES) lube_out_of_memory();
        ObjBytes *b = new_bytes((size_t)d);
        if (d > 0) memset(b->data, 0, (size_t)d);
        b->len = (size_t)d;
        return OBJ_VAL(b);
    }
    if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        ObjBytes *b = new_bytes(l->count);
        for (size_t i = 0; i < l->count; i++) {
            unsigned char c;
            if (!byte_of(l->items[i], &c)) {
                release(OBJ_VAL(b));
                return lube_error(I, "Bytes(list): item %lu is not a byte (a whole Number from 0 to 255)", (unsigned long)i);
            }
            b->data[b->len++] = c;
        }
        return OBJ_VAL(b);
    }
    return lube_error(I, "Bytes() expects a whole Number, a str, a List of bytes or Bytes, got %s", type_name(v));
}

/* A pack/unpack format: u8 i8 u16 i16 u32 i32 u64 i64 f32 f64, then be for
 * big-endian (little-endian otherwise). Its size, or 0 if it is not one. */
static int format_of(ObjString *f, char *kind, int *big) {
    const char *s = f->chars;
    size_t n = f->length;
    *big = n > 2 && !strcmp(s + n - 2, "be");
    if (*big || (n > 2 && !strcmp(s + n - 2, "le"))) n -= 2;
    if (n < 2 || (s[0] != 'u' && s[0] != 'i' && s[0] != 'f')) return 0;
    int bits = 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        bits = bits * 10 + (s[i] - '0');
        if (bits > 64) return 0;
    }
    *kind = s[0];
    if (s[0] == 'f') return bits == 32 || bits == 64 ? bits / 8 : 0;
    return bits == 8 || bits == 16 || bits == 32 || bits == 64 ? bits / 8 : 0;
}

#define BAD_FORMAT "expects a format such as \"u8\", \"i16\", \"u32be\", \"f32\" or \"f64le\""

/* b.unpack(format, at = 0): the number at byte `at` (64-bit integers: an int64 or uint64). */
NATIVE(n_unpack) {
    NEED_STR(1);
    char kind;
    int big, size = format_of(text_of(argv[1]), &kind, &big);
    if (!size) return lube_error(I, "unpack() " BAD_FORMAT ", got \"%s\"", text_of(argv[1])->chars);
    ObjBytes *b = AS_BYTES(argv[0]);
    double at = 0;
    if (argc > 2) {
        NEED_NUM(2);
        at = numeric_value(argv[2]);
    }
    if (at != floor(at) || at < 0 || at + size > (double)b->len)
        return lube_error(I, "unpack(): %s %s at %g goes past the end (length %lu)", kind == 'i' ? "an" : "a",
                          text_of(argv[1])->chars, at, (unsigned long)b->len);
    const unsigned char *p = b->data + (size_t)at;
    uint64_t u = 0;
    for (int i = 0; i < size; i++) u |= (uint64_t)p[big ? i : size - 1 - i] << (8 * (size - 1 - i));
    if (kind == 'f') {
        if (size == 4) {
            uint32_t w = (uint32_t)u;
            float x;
            memcpy(&x, &w, 4);
            return NUM_VAL((double)x);
        }
        double x;
        memcpy(&x, &u, 8);
        return NUM_VAL(x);
    }
    if (kind == 'i' && size < 8 && (u >> (8 * size - 1)) & 1) u |= ~(uint64_t)0 << (8 * size);  /* (the sign) */
    if (size == 8) return kind == 'i' ? SINT_VAL(IK_I64, (int64_t)u) : UINT_VAL(IK_U64, u);
    return NUM_VAL(kind == 'i' ? (double)(int64_t)u : (double)u);
}

/* b.pack(format, value): adds it at the end; b.pack(format, value, at): there
 * (growing b if it must). */
NATIVE(n_pack) {
    NEED_STR(1);
    char kind;
    int big, size = format_of(text_of(argv[1]), &kind, &big);
    if (!size) return lube_error(I, "pack() " BAD_FORMAT ", got \"%s\"", text_of(argv[1])->chars);
    Value v = argv[2];
    if (!IS_NUMERIC(v)) return lube_error(I, "pack() expects a Number as argument 3, got %s", type_name(v));
    uint64_t u;
    if (kind == 'f') {
        double x = numeric_value(v);
        if (size == 4) {
            float f = (float)x;
            uint32_t w;
            memcpy(&w, &f, 4);
            u = w;
        } else {
            memcpy(&u, &x, 8);
        }
    } else if (IS_INT(v)) {  /* (sized integers: as they are) */
        u = v.as.u;
    } else {
        double x = v.as.number, lo = kind == 'i' ? -ldexp(1, 8 * size - 1) : 0;
        double hi = kind == 'i' ? ldexp(1, 8 * size - 1) - 1 : ldexp(1, 8 * size) - 1;
        if (x != floor(x) || x < lo || x > hi) {
            char buf[64];
            format_number(buf, sizeof buf, x);
            return lube_error(I, "pack(): %s does not fit in %s %s", buf, kind == 'i' ? "an" : "a", text_of(argv[1])->chars);
        }
        u = x < 0 ? (uint64_t)(int64_t)x : (uint64_t)x;
    }
    double at = -1;
    if (argc > 3) {
        if (!IS_NUMERIC(argv[3]) || numeric_value(argv[3]) != floor(numeric_value(argv[3])) || numeric_value(argv[3]) < 0)
            return lube_error(I, "pack(): its position must be a whole Number, 0 or more");
        at = numeric_value(argv[3]);
        if (at > (double)LUBE_MAX_BYTES) lube_out_of_memory();
    }
    make_unique(&argv[0]);
    ObjBytes *b = AS_BYTES(argv[0]);
    size_t pos = at < 0 ? b->len : (size_t)at;
    if (pos + (size_t)size > b->len) {  /* (growing: zeros up to it) */
        bytes_reserve(b, pos + (size_t)size);
        memset(b->data + b->len, 0, pos + (size_t)size - b->len);
        b->len = pos + (size_t)size;
    }
    for (int i = 0; i < size; i++) b->data[pos + (size_t)(big ? size - 1 - i : i)] = (unsigned char)(u >> (8 * i));
    return NIL_VAL;
}

/* b.decode(): the bytes as text. */
NATIVE(n_decode) {
    (void)I;
    (void)argc;
    ObjBytes *b = AS_BYTES(argv[0]);
    return OBJ_VAL(new_string((const char *)b->data, b->len));
}

/* b.hex(): "68656c6c6f". */
NATIVE(n_hex) {
    (void)I;
    (void)argc;
    ObjBytes *b = AS_BYTES(argv[0]);
    ObjString *s = alloc_string(b->len * 2);
    for (size_t i = 0; i < b->len; i++) {
        s->chars[2 * i] = "0123456789abcdef"[b->data[i] >> 4];
        s->chars[2 * i + 1] = "0123456789abcdef"[b->data[i] & 15];
    }
    return OBJ_VAL(s);
}

/* b.slice(from, to = len): a copy of bytes [from, to); negative counts from the end. */
NATIVE(n_bslice) {
    ObjBytes *b = AS_BYTES(argv[0]);
    double len = (double)b->len, from = 0, to = len;
    for (int i = 1; i < argc; i++)
        if (!IS_NUMERIC(argv[i]) || numeric_value(argv[i]) != floor(numeric_value(argv[i])))
            return lube_error(I, "slice(): positions are whole Numbers, not %s", IS_NUMERIC(argv[i]) ? "fractions" : a_type(argv[i]));
    if (argc > 1) from = numeric_value(argv[1]);
    if (argc > 2) to = numeric_value(argv[2]);
    if (from < 0) from += len;
    if (to < 0) to += len;
    from = from < 0 ? 0 : from > len ? len : from;
    to = to < from ? from : to > len ? len : to;
    ObjBytes *out = new_bytes((size_t)(to - from));
    bytes_add(out, b->data + (size_t)from, (size_t)(to - from));
    return OBJ_VAL(out);
}

/* push(&b, x): adds a byte, or the bytes of Bytes or a str (builtins.c's push for lists). */
Value lube_bytes_push(Interp *I, int argc, Value *argv) {
    for (int i = 1; i < argc; i++) {  /* (all checked before any changes) */
        unsigned char c;
        if (!IS_BYTES(argv[i]) && !IS_TEXT(argv[i]) && !byte_of(argv[i], &c)) return not_a_byte(I, "push(): ", argv[i]);
    }
    make_unique(&argv[0]);
    ObjBytes *b = AS_BYTES(argv[0]);
    for (int i = 1; i < argc; i++) {
        unsigned char c;
        if (IS_BYTES(argv[i])) bytes_add(b, AS_BYTES(argv[i])->data, AS_BYTES(argv[i])->len);
        else if (IS_TEXT(argv[i])) bytes_add(b, text_of(argv[i])->chars, text_len(argv[i]));
        else if (byte_of(argv[i], &c)) bytes_add(b, &c, 1);
    }
    return NIL_VAL;
}

/* a + b, both Bytes. */
Value lube_bytes_concat(Value a, Value b) {
    ObjBytes *x = AS_BYTES(a), *y = AS_BYTES(b);
    if (x->len + y->len > LUBE_MAX_BYTES) lube_out_of_memory();
    ObjBytes *out = new_bytes(x->len + y->len);
    bytes_add(out, x->data, x->len);
    bytes_add(out, y->data, y->len);
    return OBJ_VAL(out);
}

/* b[k] = v (or op=): byte k (checked) of the Bytes in *slot (copied first if shared). Takes over rhs. */
Value lube_bytes_assign(Interp *I, Value *slot, size_t k, TokenType op, Value rhs) {
    ObjBytes *b = AS_BYTES(*slot);
    Value v = rhs;
    if (op != T_ASSIGN) {
        TokenType bin = op == T_PLUS_EQ ? T_PLUS : op == T_MINUS_EQ ? T_MINUS : op == T_STAR_EQ ? T_STAR
                      : op == T_SLASH_EQ ? T_SLASH : T_PERCENT;
        v = lube_arith(I, bin, NUM_VAL(b->data[k]), rhs);
        release(rhs);
        if (IS_ERR(v)) return v;
    }
    unsigned char c;
    if (!byte_of(v, &c)) {
        Value e = not_a_byte(I, "", v);
        release(v);
        return e;
    }
    release(v);
    make_unique(slot);
    AS_BYTES(*slot)->data[k] = c;
    return NUM_VAL(c);
}

/* ======================================================================
 * Time
 * ==================================================================== */

/* sleep(seconds): waits that long (a game loop's frame). */
NATIVE(n_sleep) {
    (void)argc;
    NEED_NUM(0);
    double s = numeric_value(argv[0]);
    if (!(s >= 0)) return lube_error(I, "sleep(): seconds must be 0 or more");
    if (s > 1e7) s = 1e7;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) && errno == EINTR) continue;  /* (woken early by a signal: the rest) */
#endif
    return NIL_VAL;
}

/* now(): a steady clock, in seconds (with fractions to the microsecond or
 * better), for timing frames: only the difference between two means anything. */
double lube_now(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);  /* (fixed at boot, and quick) */
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart / freq.QuadPart) + (double)(t.QuadPart % freq.QuadPart) / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

NATIVE(n_now) {
    (void)I;
    (void)argc;
    (void)argv;
    return NUM_VAL(lube_now());
}

/* ======================================================================
 * Tables
 * ==================================================================== */

static const LubeNativeDef IO[] = {
    LUBE_BUILTIN(Bytes, n_Bytes, 0, 1, 0)
    LUBE_BUILTIN(sleep, n_sleep, 1, 1, 0)
    LUBE_BUILTIN(now, n_now, 0, 0, 0)
    {NULL, NULL, 0, 0, 0}
};

static const LubeNativeDef BYTES_METHODS[] = {  /* (the receiver is argument 0) */
    LUBE_METHOD(pack, n_pack, 3, 4, 1)
    LUBE_METHOD(unpack, n_unpack, 2, 3, 0)
    LUBE_METHOD(decode, n_decode, 1, 1, 0)
    LUBE_METHOD(hex, n_hex, 1, 1, 0)
    LUBE_METHOD(slice, n_bslice, 1, 3, 0)
    {NULL, NULL, 0, 0, 0}
};

/* A type's methods (a table ending with a NULL name), by name. */
ObjMap *lube_method_table(Interp *I, const LubeNativeDef *t) {
    ObjMap *m = new_map();
    m->internal = 1;
    for (size_t i = 0; t[i].name; i++)
        map_set(m, retain(OBJ_VAL(lube_intern(I, t[i].name, strlen(t[i].name)))),
                OBJ_VAL(new_native(t[i].name, t[i].fn, t[i].min, t[i].max, t[i].changes_first)));
    return m;
}

void lube_define_natives(Interp *I, const LubeNativeDef *t) {
    for (size_t i = 0; t[i].name; i++)
        lube_define(I, t[i].name, OBJ_VAL(new_native(t[i].name, t[i].fn, t[i].min, t[i].max, t[i].changes_first)));
}

void register_io(Interp *I) {
    lube_define_natives(I, IO);
    I->tmethods[OBJ_BYTES - OBJ_BYTES] = lube_method_table(I, BYTES_METHODS);
}
