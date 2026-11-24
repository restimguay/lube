/*
 * value.c - the runtime data types: strings, lists, ordered maps, ranges and
 * functions, plus copy-on-write, truthiness, equality and printing.
 *
 * Ownership conventions:
 *   list_push(l, v)      takes over the reference to v
 *   map_set(m, k, v)     takes over the references to k and v
 *   map_get(m, k, &out)  out is borrowed (not retained)
 *   new_* / *_copy       return a new reference (count 1)
 */
#include "lube.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Strings
 * ==================================================================== */

uint32_t hash_bytes(const char *s, size_t n) {  /* FNV-1a */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

ObjString *alloc_string(size_t length) {
    if (length > LUBE_MAX_BYTES) lube_out_of_memory();
    ObjString *s = (ObjString *)alloc_object(sizeof(ObjString) + length + 1, OBJ_STRING);
    s->length = length;
    s->capacity = length;
    s->hash = 0;
    s->hashed = 0;
    s->chars[length] = '\0';
    return s;
}

ObjString *new_string(const char *chars, size_t length) {
    ObjString *s = alloc_string(length);
    if (length) memcpy(s->chars, chars, length);
    return s;
}

/* The hash of a text: a short one's as sstr_hash gives it (so a str hashes the same held either way). */
uint32_t text_hash(const char *chars, size_t length) {
    return length <= LUBE_SSTR_MAX ? sstr_hash(sstr_value(chars, length)) : hash_bytes(chars, length);
}

uint32_t string_hash(ObjString *s) {
    if (!s->hashed) {
        s->hash = text_hash(s->chars, s->length);
        s->hashed = 1;
    }
    return s->hash;
}

Value str_value(const char *chars, size_t length) {
    return length <= LUBE_SSTR_MAX ? sstr_value(chars, length) : OBJ_VAL(new_string(chars, length));
}

Value str_take(ObjString *s) {
    if (s->length > LUBE_SSTR_MAX) return OBJ_VAL(s);
    Value v = sstr_value(s->chars, s->length);
    release(OBJ_VAL(s));
    return v;
}

ObjString *str_object(Value v) {
    if (IS_SSTR(v)) return new_string(SSTR_CHARS(&v), SSTR_LEN(v));
    ObjString *s = IS_HSTR(v) ? AS_STRING(v) : AS_STROBJ(v)->text;
    s->obj.rc++;
    return s;
}

Value str_heap(Value v) { return IS_SSTR(v) ? OBJ_VAL(new_string(SSTR_CHARS(&v), SSTR_LEN(v))) : v; }

int text_equal(Value a, Value b) {
    if (IS_SSTR(a) && IS_SSTR(b)) return sstr_same(a, b);
    size_t la, lb;
    const char *x = text_chars(&a, &la), *y = text_chars(&b, &lb);
    return la == lb && (x == y || memcmp(x, y, la) == 0);
}

/* Appends to a string that has exactly one owner (the caller checks).
 * The string may move, so the caller must use the returned pointer. */
ObjString *string_append(ObjString *s, const char *chars, size_t n) {
    if (n > LUBE_MAX_BYTES - s->length) lube_out_of_memory();
    if (s->length + n > s->capacity) {
        size_t cap = s->capacity < 16 ? 16 : s->capacity;
        while (cap < s->length + n) cap *= 2;
        uintptr_t from = (uintptr_t)chars, lo = (uintptr_t)s->chars;
        int inside = from >= lo && from <= lo + s->length;  /* appending part of itself */
        size_t offset = (size_t)(from - lo);
        s = (ObjString *)realloc_object(&s->obj, sizeof(ObjString) + s->capacity + 1, sizeof(ObjString) + cap + 1);
        s->capacity = cap;
        if (inside) chars = s->chars + offset;
    }
    memmove(s->chars + s->length, chars, n);
    s->length += n;
    s->chars[s->length] = '\0';
    s->hashed = 0;
    return s;
}

/* A map's index is searched from slot hash & mask, then along this: 5 i + 1 comes to every slot of a table
 * whose size is a power of two, and the hash's higher bits (perturb, shifted in 5 at a time until none is
 * left) join it first, so that keys alike in their lower bits part ways (as Python's dict). Whole numbers
 * hash to themselves: keys 0, 1, 2... fill the index in order, with no collisions and no cache misses. */
#define PROBE_NEXT(i, perturb, mask) ((perturb) >>= 5, (i) = ((i) * 5 + (perturb) + 1) & (mask))

/* Returns the shared copy of a string. The table owns it; the caller gets a
 * borrowed pointer that stays valid until the interpreter shuts down. */
ObjString *lube_intern(Interp *I, const char *chars, size_t length) {
    uint32_t h = text_hash(chars, length);
    ObjMap *m = I->strings;
    if (m->index_cap) {
        size_t mask = m->index_cap - 1, i = h & mask;
        uint32_t perturb = h;
        for (;;) {
            int32_t e = m->index[i];
            if (e == -1) break;
            if (e >= 0 && m->entries[e].hash == h) {
                ObjString *s = AS_STRING(m->entries[e].key);
                if (s->length == length && memcmp(s->chars, chars, length) == 0) return s;
            }
            PROBE_NEXT(i, perturb, mask);
        }
    }
    ObjString *s = new_string(chars, length);
    map_set(m, OBJ_VAL(s), BOOL_VAL(1));
    return s;
}

#if LUBE_SWAR_SCAN
/* The first i (going 8 at a time, while i + 7 <= w) where some place of i..i+7 has the needle's first byte and,
 * `last` bytes on, its last byte; or where it stopped. (On its own, so the loop is ten instructions a word.) */
static LUBE_NOINLINE size_t pair_scan(const unsigned char *s, size_t i, size_t w, size_t last, uint64_t heads,
                                      uint64_t tails) {
    for (; i + 15 <= w; i += 16) {  /* (two words a turn) */
        uint64_t x = (lube_load64((const char *)s + i) ^ heads) | (lube_load64((const char *)s + i + last) ^ tails);
        uint64_t y = (lube_load64((const char *)s + i + 8) ^ heads) | (lube_load64((const char *)s + i + 8 + last) ^ tails);
        if ((((x - LUBE_W_ONES) & ~x) | ((y - LUBE_W_ONES) & ~y)) & LUBE_W_HIGH) break;
    }
    for (; i + 7 <= w; i += 8) {
        uint64_t x = (lube_load64((const char *)s + i) ^ heads) | (lube_load64((const char *)s + i + last) ^ tails);
        if ((x - LUBE_W_ONES) & ~x & LUBE_W_HIGH) break;  /* (a byte of x is zero: a place has both) */
    }
    return i;
}
#endif

/* Where needle first is in hay at or after `from`, or -1.
 *
 * One byte: the first 32 bytes a word at a time (most searches end there: the pieces of a split, a short str),
 * the rest by memchr. Longer needles: memchr hops from one first byte to the next while those hops are long (a
 * first byte that is rare in this text: nothing is quicker); once they are short, the needle's first and last
 * bytes are both looked for in eight places at once (two words of the text, as far apart as those two bytes),
 * and only a place where both match is compared in full. Without LUBE_SWAR_SCAN that second part is the search
 * CPython's str.find makes (Sunday's skip with a 64-bit set of the needle's bytes), which finds the same places. */
static LUBE_NOINLINE long find_long(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t from);

long str_find(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t from) {
    if (nlen == 0) return from <= hlen ? (long)from : -1;
    if (nlen > hlen || from > hlen - nlen) return -1;
    if (hlen - from > 16 || nlen == 1) return find_long(hay, hlen, needle, nlen, from);
    /* a short text ("99" in "1995"): place by place, with nothing to set up */
    const unsigned char *s = (const unsigned char *)hay + from, *p = (const unsigned char *)needle;
    size_t last = nlen - 1, w = hlen - from - nlen;
    for (size_t i = 0; i <= w; i++) {
        if (s[i] != p[0] || s[i + last] != p[last]) continue;
        size_t j = 1;
        while (j < last && s[i + j] == p[j]) j++;
        if (j >= last) return (long)(from + i);
    }
    return -1;
}

static LUBE_NOINLINE long find_long(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t from) {
    const unsigned char *s = (const unsigned char *)hay + from, *p = (const unsigned char *)needle;
    size_t n = hlen - from;
    if (nlen == 1) {
        size_t head = n < 32 ? n : 32, i = lube_scan_byte((const char *)s, head, p[0]);
        if (i < head) return (long)(from + i);
        if (head == n) return -1;
        const char *q = memchr(s + head, p[0], n - head);
        return q ? (long)(q - hay) : -1;
    }
    size_t last = nlen - 1, w = n - nlen, i = 0;  /* w: the last place a match can start */
    unsigned char head = p[0], tail = p[last];
#define MATCH_AT(at) (s[(at) + last] == tail && memcmp(s + (at) + 1, p + 1, last - 1) == 0)
    if (w >= 64) {  /* (a long text: is the first byte rare in it?) */
        for (int short_hops = 0; short_hops < 4 && i <= w;) {
            const unsigned char *q = memchr(s + i, head, w - i + 1);
            if (!q) return -1;
            size_t at = (size_t)(q - s);
            if (MATCH_AT(at)) return (long)(from + at);
            short_hops += at - i < 32;
            i = at + 1;
        }
    }
#if LUBE_SWAR_SCAN
    while (i + 7 <= w) {  /* places i..i+7: each needs s[..+last], so the second word ends at w + last = n - 1 */
        i = pair_scan(s, i, w, last, LUBE_W_ONES * head, LUBE_W_ONES * tail);
        if (i + 7 > w) break;
        uint64_t x = (lube_load64((const char *)s + i) ^ (LUBE_W_ONES * head)) |
                     (lube_load64((const char *)s + i + last) ^ (LUBE_W_ONES * tail));
        for (uint64_t m = lube_zero_bytes(x); m; m &= m - 1) {
            size_t at = i + LUBE_FIRST_BYTE(m);
            if (s[at + 1] == p[1] && memcmp(s + at + 1, p + 1, last - 1) == 0) return (long)(from + at);
        }
        i += 8;
    }
    for (; i <= w; i++)
        if (s[i] == head && MATCH_AT(i)) return (long)(from + i);
#else
    size_t skip = last;
    uint64_t set = (uint64_t)1 << (tail & 63);
    for (size_t k = 0; k < last; k++) {
        set |= (uint64_t)1 << (p[k] & 63);
        if (p[k] == tail) skip = last - k - 1;  /* (how far the window may move when its last byte matched) */
    }
    const unsigned char *ends = s + last;  /* ends[i]: the last byte of the window at i */
    for (; i <= w; i++) {
        if (ends[i] == tail) {
            if (s[i] == head && MATCH_AT(i)) return (long)(from + i);
            if (i < w && !((set >> (ends[i + 1] & 63)) & 1)) i += nlen;
            else i += skip;
        } else if (i < w && !((set >> (ends[i + 1] & 63)) & 1)) {
            i += nlen;
        }
    }
#endif
#undef MATCH_AT
    return -1;
}

/* How many bytes of s[0..n) are c: eight at a time (a byte that matches leaves 1 in its place, and one
 * multiplication adds the eight). */
size_t str_count_byte(const char *s, size_t n, unsigned char c) {
    size_t count = 0, i = 0;
    uint64_t pattern = LUBE_W_ONES * c;
    for (; i + 8 <= n; i += 8)
        count += (size_t)((((lube_zero_bytes(lube_load64(s + i) ^ pattern)) >> 7) * LUBE_W_ONES) >> 56);
    for (; i < n; i++) count += (unsigned char)s[i] == c;
    return count;
}

/* How many times needle is in hay, counting from the left without overlaps (as split and replace take them). */
size_t str_count(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 1) return str_count_byte(hay, hlen, (unsigned char)needle[0]);
    size_t count = 0, start = 0;
    long pos;
    while ((pos = str_find(hay, hlen, needle, nlen, start)) >= 0) {
        count++;
        start = (size_t)pos + nlen;
    }
    return count;
}

/* dst[0..n) = src[0..n) with its ASCII letters in upper (or lower) case, eight bytes at a time: a byte is a
 * letter to change when its low 7 bits are in a..z (A..Z) and its high bit is clear (so UTF-8's bytes stay);
 * the two cases differ in bit 0x20. */
void str_change_case(char *dst, const char *src, size_t n, int upper) {
    unsigned char first = upper ? 'a' : 'A';
    size_t i = 0;
#ifndef LUBE_LOOPS_VECTORIZED  /* (see lube.h: where the compiler makes the plain loop below 16 bytes at a time) */
    uint64_t from_first = LUBE_W_ONES * (0x80u - first), past_last = LUBE_W_ONES * (0x7fu - (first + 25u));
    for (; i + 8 <= n; i += 8) {
        uint64_t w = lube_load64(src + i), low = w & LUBE_W_LOW7;
        w ^= ((low + from_first) & ~(low + past_last) & ~w & LUBE_W_HIGH) >> 2;
        memcpy(dst + i, &w, 8);
    }
#endif
    for (; i < n; i++) {  /* (no branch: a letter of the case to change has its bit 0x20 flipped) */
        unsigned char c = (unsigned char)src[i];
        dst[i] = (char)(c ^ (((unsigned char)(c - first) < 26) << 5));
    }
}

/* The Number written in s[0..n) exactly as [-]digits[.digits], with at most 15 digits in all: such a number is
 * a whole number below 2^53 divided by a power of ten of at most 10^15, both exact as doubles, so one division
 * gives the correctly rounded result: the double strtod gives. Returns 0 for anything else (an exponent, more
 * digits, inf, hex, spaces around): strtod's to read. */
int str_to_double(const char *s, size_t n, double *out) {
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != 1
    (void)s; (void)n; (void)out;
    return 0;  /* (arithmetic wider than double would round twice: strtod's, always) */
#else
    static const double pow10[16] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    size_t i = 0;
    int neg = n && s[0] == '-';
    if (neg) i = 1;
    if (n - i == 0 || n - i > 16) return 0;
    uint64_t mant = 0;
    int digits = 0, frac = -1;  /* frac: digits after the point (-1: no point yet) */
    for (; i < n; i++) {
        unsigned d = (unsigned char)s[i] - '0';
        if (d < 10) {
            mant = mant * 10 + d;
            digits++;
            if (frac >= 0) frac++;
        } else if (s[i] == '.' && frac < 0) {
            frac = 0;
        } else {
            return 0;
        }
    }
    if (digits == 0 || digits > 15 || frac == 0) return 0;  /* ("1." and ".": strtod's) */
    double d = (double)mant;
    if (frac > 0) d /= pow10[frac];
    *out = neg ? -d : d;
    return 1;
#endif
}

size_t utf8_seq_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;  /* invalid lead byte: treat as a single byte */
}

/* ======================================================================
 * Lists
 * ==================================================================== */

ObjList *new_list(void) {
    ObjList *l = (ObjList *)alloc_object(sizeof(ObjList), OBJ_LIST);
    l->count = l->capacity = 0;
    l->items = NULL;
    l->elem = TT_ANY;
    return l;
}

void list_reserve(ObjList *l, size_t n) {
    if (n <= l->capacity) return;
    if (n > LUBE_MAX_ITEMS) lube_out_of_memory();
    size_t cap = l->capacity ? l->capacity * 2 : n > 1 ? n : 4;  /* (a literal's items: just enough) */
    while (cap < n) cap *= 2;
    l->items = lube_block_realloc(l->items, l->capacity * sizeof(Value), cap * sizeof(Value));
    l->capacity = cap;
}

#ifdef LUBE_CHECK_LEAKS
void lube_temp_kept(void) {
    fputs("lube: a StrTemp's ObjString was kept (retained, or put in a list or a map)\n", stderr);
    abort();
}
#endif

void list_push(ObjList *l, Value v) {
    LUBE_NOT_TEMP(v);
    list_reserve(l, l->count + 1);
    l->items[l->count++] = v;
}

/* Is any of items[0..n) an object (something with an owner count)? A list of Numbers, Bools and short strs has
 * none, and then copying it is one memcpy and freeing it is freeing its block. (No branch in the loop: a
 * compiler can test several items at once.) */
int lube_any_objects(const Value *items, size_t n) {
    unsigned any = 0;
    for (size_t i = 0; i < n; i++) any |= (unsigned)(items[i].type == VAL_OBJ);
    return (int)any;
}

/* Appends n items (borrowed: each gets one owner more) to l, which has room for them: one copy of the block,
 * then the owner counts of those that have one (a list of Numbers has none). */
void list_take(ObjList *l, const Value *items, size_t n) {
    Value *to = l->items + l->count;
    if (n) memcpy(to, items, n * sizeof(Value));
    if (n <= 16 || lube_any_objects(to, n))  /* (a short list: looked at once) */
        for (size_t i = 0; i < n; i++)
            if (to[i].type == VAL_OBJ) to[i].as.obj->rc++;
    l->count += n;
}

ObjList *list_copy(ObjList *src) {
    ObjList *l = new_list();
    list_reserve(l, src->count);
    list_take(l, src->items, src->count);
    l->elem = src->elem;  /* a copy of a String[] is still a String[] */
    return l;
}

/* ======================================================================
 * Maps: entries are kept in insertion order; `index` is an open addressing
 * table (linear probing) holding positions into `entries`.
 * ==================================================================== */

#define SLOT_EMPTY (-1)
#define SLOT_DELETED (-2)

/* Map keys are restricted to values that compare by content. */
/* Keys an Object accepts. */
int is_key(Value v) { return IS_BOOL(v) || IS_NUMERIC(v) || IS_STRING(v); }

/* Keys this Object or Map accepts: an Object takes String, Number and Bool
 * keys; a Map takes any key, or only its key type for Map[K, V]. */
int map_key_ok(ObjMap *m, Value key) { return m->dict ? value_has_type(key, m->ktype) : is_key(key); }

static uint32_t mix64(uint64_t bits) {
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 33;
    return (uint32_t)bits;
}

static uint32_t hash_double(double d) {
    if (d == 0) d = 0;  /* -0 and 0 are the same key */
    if (d >= -9007199254740992.0 && d <= 9007199254740992.0 && d == (double)(int64_t)d) {
        uint64_t u = (uint64_t)(int64_t)d;  /* a whole number: itself (see PROBE_NEXT), its upper half mixed in */
        return (uint32_t)u + (uint32_t)(u >> 32) * 0x9e3779b9u;
    }
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return mix64(bits);
}

#define HASH_DEPTH 16  /* deeper parts of a key are compared, not hashed */

/* Equal keys must hash alike: 5, int32(5) and uint8(5) share a hash, a List
 * hashes its items in order, and a Map or Object hashes its entries in any
 * order (as == compares them). Map keys can never change once stored (value
 * semantics), so a stored hash stays valid. */
static uint32_t hash_at(Value v, int depth) {
    double d;
    if (IS_SSTR(v)) return sstr_hash(v);
    if (IS_TEXT(v)) return string_hash(IS_HSTR(v) ? AS_STRING(v) : AS_STROBJ(v)->text);  /* "a" == String("a") */
    if (IS_BOOL(v)) return v.as.boolean ? 0x2545f491u : 0x6c8e9cf5u;
    if (IS_NUM(v)) return hash_double(v.as.number);
    if (IS_INT(v)) return int_exact_double(v, &d) ? hash_double(d) : mix64(v.as.u ^ 0x5bd1e995u);
    uint32_t h = (uint32_t)OBJ_TYPE(v) * 0x9e3779b9u;
    if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        h ^= (uint32_t)l->count;
        for (size_t i = 0; i < l->count && depth < HASH_DEPTH; i++) {  /* (Numbers and short strs: no call) */
            Value item = l->items[i];
            h = h * 31u + (IS_NUM(item) ? hash_double(item.as.number) : IS_SSTR(item) ? sstr_hash(item)
                                                                                    : hash_at(item, depth + 1));
        }
    } else if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        h ^= (uint32_t)m->count ^ (uint32_t)((uintptr_t)m->klass >> 4);
        for (size_t i = 0; i < m->used && depth < HASH_DEPTH; i++)
            if (m->entries[i].live)  /* a sum does not depend on the order */
                h += mix64(((uint64_t)hash_at(m->entries[i].key, depth + 1) << 32) | hash_at(m->entries[i].value, depth + 1));
    } else if (IS_RANGE(v)) {
        ObjRange *r = AS_RANGE(v);
        h ^= hash_double(r->start) + 3u * hash_double(r->end) + 7u * hash_double(r->step) + (uint32_t)r->inclusive;
    } else if (IS_BYTES(v)) {
        ObjBytes *b = AS_BYTES(v);
        h ^= hash_bytes((const char *)b->data, b->len);
    } else {
        h ^= mix64((uint64_t)(uintptr_t)v.as.obj);  /* functions: by identity */
    }
    return h;
}

/* (A short str: two multiplications. A name, or any ObjString looked up before: the hash it keeps. No call.) */
static inline uint32_t hash_value(Value v) {
    if (IS_SSTR(v)) return sstr_hash(v);
    if (IS_HSTR(v) && AS_STRING(v)->hashed) return AS_STRING(v)->hash;
    return hash_at(v, 0);
}

/* Called only when the hashes match. */
static int keys_equal(Value a, Value b) {
    if (IS_OBJ(a) && IS_OBJ(b) && a.as.obj == b.as.obj) return 1;  /* (a name, interned: itself) */
    if (IS_SSTR(a) && IS_SSTR(b)) return sstr_same(a, b);
    if (IS_TEXT(a) && IS_TEXT(b)) return text_equal(a, b);
    if (IS_LIST(a) && IS_LIST(b)) {  /* a key such as [x, y]: its Numbers and short strs compared where they are */
        ObjList *x = AS_LIST(a), *y = AS_LIST(b);
        if (x->count != y->count) return 0;
        size_t i = 0;
        for (; i < x->count; i++) {
            Value p = x->items[i], q = y->items[i];
            if (IS_NUM(p) && IS_NUM(q)) {
                if (p.as.number != q.as.number) return 0;
            } else if (IS_SSTR(p) && IS_SSTR(q)) {
                if (!sstr_same(p, q)) return 0;
            } else {
                break;  /* (anything else: the general comparison, of the whole key) */
            }
        }
        if (i == x->count) return 1;
    }
    return values_equal(a, b);  /* numbers by value, others by contents */
}

ObjMap *new_map(void) {
    ObjMap *m = (ObjMap *)alloc_object(sizeof(ObjMap), OBJ_MAP);
    m->entries = NULL;
    m->used = m->count = m->entry_cap = 0;
    m->index = NULL;
    m->index_cap = 0;
    m->dict = 0;
    m->ktype = m->vtype = TT_ANY;
    m->internal = 0;
    m->klass = NULL;
    return m;
}

/* Returns the index-table slot holding `key`, or -1. */
static long map_probe(ObjMap *m, Value key, uint32_t h) {
    if (m->index_cap == 0) return -1;
    size_t mask = m->index_cap - 1, i = h & mask;
    uint32_t perturb = h;
    for (;;) {
        int32_t e = m->index[i];
        if (e == SLOT_EMPTY) return -1;
        if (e >= 0 && m->entries[e].hash == h && keys_equal(m->entries[e].key, key)) return (long)i;
        PROBE_NEXT(i, perturb, mask);
    }
}

/* Drops deleted entries, resizes to hold at least `want` entries and
 * rebuilds the index, which is kept at most half full. */
static void map_resize(ObjMap *m, size_t want) {
    size_t j = 0;
    for (size_t i = 0; i < m->used; i++) {
        if (!m->entries[i].live) continue;
        if (i != j) m->entries[j] = m->entries[i];
        j++;
    }
    m->used = j;

    /* index entries are 32-bit, and sizes must not overflow (the limit is on items, not on room) */
    if (want > ((size_t)1 << 30) || (m->count >= LUBE_MAX_ITEMS && !m->internal)) lube_out_of_memory();
    size_t cap = 4;  /* (small ones, and their index, come from memory.c's free lists) */
    while (cap < want) cap *= 2;
    m->entries = lube_block_realloc(m->entries, m->entry_cap * sizeof(MapEntry), cap * sizeof(MapEntry));
    m->entry_cap = cap;
    size_t icap = cap * 2;
    lube_block_free(m->index, m->index_cap * sizeof(int32_t));
    m->index = lube_block_alloc(icap * sizeof(int32_t));
    m->index_cap = icap;
    for (size_t i = 0; i < icap; i++) m->index[i] = SLOT_EMPTY;
    size_t mask = icap - 1;
    for (size_t e = 0; e < m->used; e++) {
        uint32_t perturb = m->entries[e].hash;
        size_t i = perturb & mask;
        while (m->index[i] != SLOT_EMPTY) PROBE_NEXT(i, perturb, mask);
        m->index[i] = (int32_t)e;
    }
}

/* Room for n entries in all, made at once (an instance's fields, an object written out). */
void map_reserve(ObjMap *m, size_t n) {
    if (n > m->entry_cap) map_resize(m, n);
}

/* A new map's first n entries: keys[i] (strs, no two alike: a class's field names) with values[i], both
 * retained. No key is looked for, and each hash is its str's own. */
void map_fill(ObjMap *m, ObjString **keys, const Value *values, size_t n) {
    if (!n) return;
    map_reserve(m, n);
    size_t mask = m->index_cap - 1;
    for (size_t e = 0; e < n; e++) {
        uint32_t h = string_hash(keys[e]);
        uint32_t perturb = h;
        size_t i = h & mask;
        while (m->index[i] != SLOT_EMPTY) PROBE_NEXT(i, perturb, mask);
        m->index[i] = (int32_t)e;
        m->entries[e].key = retain(OBJ_VAL(keys[e]));
        m->entries[e].value = retain(values[e]);
        m->entries[e].hash = h;
        m->entries[e].live = 1;
    }
    m->used = m->count = n;
}

Value *map_find(ObjMap *m, Value key) {
    long slot = map_probe(m, key, hash_value(key));
    return slot < 0 ? NULL : &m->entries[m->index[slot]].value;
}

int map_get(ObjMap *m, Value key, Value *out) {
    Value *v = map_find(m, key);
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}

/* The slot of key's value, added (holding nothing, the key retained) when it is not there: one hash and one
 * probe for both. */
Value *map_slot(ObjMap *m, Value key) {
    uint32_t h = hash_value(key);
    if (m->index_cap) {
        size_t mask = m->index_cap - 1, i = h & mask;
        uint32_t perturb = h;
        for (;;) {
            int32_t e = m->index[i];
            if (e == SLOT_EMPTY) {
                if (m->used >= m->entry_cap) break;  /* (no room: map_set makes it) */
                MapEntry *n = &m->entries[m->used];
                n->key = retain(key);
                n->value = NIL_VAL;
                n->hash = h;
                n->live = 1;
                m->index[i] = (int32_t)m->used;
                m->used++;
                m->count++;
                return &n->value;
            }
            if (e >= 0 && m->entries[e].hash == h && keys_equal(m->entries[e].key, key)) return &m->entries[e].value;
            PROBE_NEXT(i, perturb, mask);
        }
    }
    map_set(m, retain(key), NIL_VAL);  /* (not there, so added: the last entry) */
    return &m->entries[m->used - 1].value;
}

void map_set(ObjMap *m, Value key, Value value) {
    LUBE_NOT_TEMP(key);
    LUBE_NOT_TEMP(value);
    uint32_t h = hash_value(key);
    long slot = map_probe(m, key, h);
    if (slot >= 0) {
        MapEntry *e = &m->entries[m->index[slot]];
        Value old = e->value;
        e->value = value;
        release(old);
        release(key);
        return;
    }
    if (m->used >= m->entry_cap) map_resize(m, m->count + 1 + m->count / 2);
    size_t mask = m->index_cap - 1, i = h & mask;
    uint32_t perturb = h;
    while (m->index[i] >= 0) PROBE_NEXT(i, perturb, mask);  /* first empty or deleted slot */
    MapEntry *e = &m->entries[m->used];
    e->key = key;
    e->value = value;
    e->hash = h;
    e->live = 1;
    m->index[i] = (int32_t)m->used;
    m->used++;
    m->count++;
}

/* Removes `key`. The removed value is handed to *removed if given (the
 * caller then owns it), otherwise released. Returns 0 if it was missing. */
int map_delete(ObjMap *m, Value key, Value *removed) {
    long slot = map_probe(m, key, hash_value(key));
    if (slot < 0) return 0;
    MapEntry *e = &m->entries[m->index[slot]];
    Value old_key = e->key, old_value = e->value;
    e->live = 0;
    e->key = NIL_VAL;
    e->value = NIL_VAL;
    m->index[slot] = SLOT_DELETED;
    m->count--;
    if (m->count == 0) {  /* empty again: forget the tombstones */
        m->used = 0;
        for (size_t i = 0; i < m->index_cap; i++) m->index[i] = SLOT_EMPTY;
    }
    release(old_key);
    if (removed) *removed = old_value;
    else release(old_value);
    return 1;
}

ObjMap *map_copy(ObjMap *src) {
    ObjMap *m = new_map();
    m->dict = src->dict;  /* a copy of a Map[K, V] (or of an instance) keeps its kind */
    m->ktype = src->ktype;
    m->vtype = src->vtype;
    m->klass = src->klass;
    if (src->count) map_resize(m, src->count);
    for (size_t i = 0; i < src->used; i++) {
        MapEntry *e = &src->entries[i];
        if (e->live) map_set(m, retain(e->key), retain(e->value));
    }
    return m;
}

/* Copy-on-write: before changing a list or map in place, make sure this
 * slot is its only owner. If it is shared, the slot gets its own copy. */
void make_unique(Value *slot) {
    /* only lists, objects and bytes are ever changed in place */
    if (!(IS_LIST(*slot) || IS_MAP(*slot) || IS_BYTES(*slot)) || slot->as.obj->rc <= 1) return;
    Value copy;
    if (IS_BYTES(*slot)) {
        ObjBytes *b = AS_BYTES(*slot), *c = new_bytes(b->len);
        if (b->len) memcpy(c->data, b->data, b->len);
        c->len = b->len;
        copy = OBJ_VAL(c);
    } else {
        copy = IS_LIST(*slot) ? OBJ_VAL(list_copy(AS_LIST(*slot))) : OBJ_VAL(map_copy(AS_MAP(*slot)));
    }
    release(*slot);
    *slot = copy;
}

/* ======================================================================
 * Functions and ranges
 * ==================================================================== */

ObjFunction *new_function(FuncDecl *decl, int ncaptures) {
    ObjFunction *f = (ObjFunction *)alloc_object(sizeof(ObjFunction) + (size_t)ncaptures * sizeof(Value),
                                                 OBJ_FUNCTION);
    f->decl = decl;
    f->ncaptures = ncaptures;
    for (int i = 0; i < ncaptures; i++) f->captures[i] = NIL_VAL;
    return f;
}

ObjNative *new_native(const char *name, NativeFn fn, int min, int max, int changes_first) {
    ObjNative *n = (ObjNative *)alloc_object(sizeof(ObjNative), OBJ_NATIVE);
    n->name = name;
    n->fn = fn;
    n->min_args = min;
    n->max_args = max;
    n->changes_first = changes_first;
    n->plugin_fn = NULL;
    n->cell = NULL;
    n->od = NULL;
    return n;
}

/* A String object wrapping text (takes over the reference to text). */
ObjStrObj *new_strobj(ObjString *text) {
    ObjStrObj *s = (ObjStrObj *)alloc_object(sizeof(ObjStrObj), OBJ_STROBJ);
    s->text = text;
    return s;
}

ObjRange *new_range(double start, double end, double step, int inclusive) {
    ObjRange *r = (ObjRange *)alloc_object(sizeof(ObjRange), OBJ_RANGE);
    r->start = start;
    r->end = end;
    r->step = step;
    r->inclusive = inclusive;
    return r;
}

double range_length(ObjRange *r) {
    double span = (r->end - r->start) / r->step;
    if (span < 0) return 0;
    double n = r->inclusive ? floor(span + 1e-9) + 1 : ceil(span - 1e-9);
    return n > 0 ? n : 0;  /* also 0 for NaN */
}

/* ======================================================================
 * Truthiness, equality, type names
 * ==================================================================== */

/* ======================================================================
 * Type names
 * ==================================================================== */

static const char *TYPE_NAMES[TT_COUNT] = {
    "List", "str", "String", "Number", "Bool", "Object", "Map", "List", "Function", "Range", "Channel", "Isolate",
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "Bytes", "Money",
};
static const char *LIST_NAMES[TT_COUNT] = {
    "List", "str[]", "String[]", "Number[]", "Bool[]", "Object[]", "Map[]", "List[]", "Function[]", "Range[]",
    "Channel[]", "Isolate[]", "int8[]", "int16[]", "int32[]", "int64[]", "uint8[]", "uint16[]", "uint32[]", "uint64[]",
    "Bytes[]", "Money[]",
};

/* The TypeTag of a type name such as "String" or "int32", or -1. */
int type_tag_from_name(const char *s, size_t n) {
    for (int t = TT_STRING; t < TT_COUNT; t++)
        if (strlen(TYPE_NAMES[t]) == n && memcmp(TYPE_NAMES[t], s, n) == 0) return t;
    return -1;
}

const char *tag_name(int tag) { return TYPE_NAMES[tag]; }

/* "Map" or "Map[Object, String]" (built once per combination). */
static const char *map_name(int ktype, int vtype) {
    static LUBE_THREAD_LOCAL char names[TT_COUNT][TT_COUNT][48];
    if (ktype == TT_ANY) return "Map";
    char *s = names[ktype][vtype];
    if (!s[0]) snprintf(s, sizeof names[0][0], "Map[%s, %s]", TYPE_NAMES[ktype], TYPE_NAMES[vtype]);
    return s;
}

/* "a String", "an Object", "an int32": for error messages. A few rotating
 * buffers let one message use it several times. */
const char *with_article(const char *name) {
    static LUBE_THREAD_LOCAL char bufs[4][80];
    static LUBE_THREAD_LOCAL int next = 0;
    char *b = bufs[next];
    next = (next + 1) % 4;
    snprintf(b, sizeof bufs[0], "%s %s", strchr("AEIOUaeiou", name[0]) ? "an" : "a", name);
    return b;
}

int is_subclass(ObjClass *c, ObjClass *ancestor) {
    for (; c; c = c->parent)
        if (c == ancestor) return 1;
    return 0;
}

/* The declared type a value gives a var that starts with it. */
TypeDesc desc_of(Value v) {
    TypeDesc d = {TD_NUM, 0, 0, 0, NULL};
    if (IS_BOOL(v)) d.kind = TD_BOOL;
    else if (IS_SSTR(v)) d.kind = TD_STR;
    else if (IS_INT(v)) { d.kind = TD_INT; d.a = v.ikind; }
    else if (IS_OBJ(v)) {
        switch (OBJ_TYPE(v)) {
        case OBJ_STRING: d.kind = TD_STR; break;
        case OBJ_STROBJ: d.kind = TD_STROBJ; break;
        case OBJ_LIST: d.kind = TD_LIST; d.a = (unsigned char)AS_LIST(v)->elem; break;
        case OBJ_RANGE: d.kind = TD_RANGE; break;
        case OBJ_CLASS: d.kind = TD_CLASSVAL; break;
        case OBJ_CHANNEL: d.kind = TD_CHANNEL; break;
        case OBJ_BYTES: d.kind = TD_BYTES; break;
        case OBJ_MONEY: d.kind = TD_MONEY; break;
        case OBJ_FOREIGN: d.kind = TD_CLASS; d.cls = AS_FOREIGN(v)->klass; break;
        case OBJ_ISOLATE: d.kind = TD_ISOLATE; break;
        case OBJ_PACKAGE: case OBJ_COMPONENT: d.kind = TD_OBJECT; break;
        case OBJ_MAP:
            if (AS_MAP(v)->klass) { d.kind = TD_CLASS; d.cls = AS_MAP(v)->klass; }
            else if (AS_MAP(v)->dict) { d.kind = TD_MAP; d.a = AS_MAP(v)->ktype; d.b = AS_MAP(v)->vtype; }
            else d.kind = TD_OBJECT;  /* a plain object: the var can hold any object */
            break;
        default: d.kind = TD_FUNCTION; break;
        }
    }
    return d;
}

/* Does declared type d accept v? *out receives the value to store: v itself,
 * or a plain whole number turned into d's integer type. */
int desc_accepts(TypeDesc d, Value v, Value *out) {
    *out = v;
    switch (d.kind) {
    case TD_NUM: return IS_NUM(v);
    case TD_BOOL: return IS_BOOL(v);
    case TD_INT:
        if (IS_INT(v)) return v.ikind == d.a;
        return IS_NUM(v) && int_try_whole(v.as.number, d.a, out);
    case TD_STR: return IS_STRING(v);
    case TD_STROBJ: return IS_STROBJ(v);
    case TD_LIST: return IS_LIST(v) && (d.any || AS_LIST(v)->elem == d.a);
    case TD_MAP:
        return IS_MAP(v) && AS_MAP(v)->dict && (d.any || (AS_MAP(v)->ktype == d.a && AS_MAP(v)->vtype == d.b));
    case TD_OBJECT: return IS_OBJ(v) && !IS_STRING(v);
    case TD_FUNCTION: return IS_CALLABLE(v);
    case TD_RANGE: return IS_RANGE(v);
    case TD_CHANNEL: return IS_CHANNEL(v);
    case TD_BYTES: return IS_BYTES(v);
    case TD_MONEY: return IS_MONEY(v);
    case TD_ISOLATE: return IS_ISOLATE(v);
    case TD_CLASSVAL: return IS_CLASS(v);
    default:  /* TD_CLASS: an instance of it or of a subclass; or a plugin's object of its type */
        if (d.cls->foreign) return IS_FOREIGN(v) && AS_FOREIGN(v)->klass->foreign == d.cls->foreign;
        return IS_INSTANCE(v) && is_subclass(AS_MAP(v)->klass, d.cls);
    }
}

const char *desc_name(TypeDesc d) {
    switch (d.kind) {
    case TD_NUM: return "Number";
    case TD_BOOL: return "Bool";
    case TD_INT: return int_type_name(d.a);
    case TD_STR: return "str";
    case TD_STROBJ: return "String";
    case TD_LIST: return LIST_NAMES[d.a];
    case TD_MAP: return map_name(d.a, d.b);
    case TD_OBJECT: return "Object";
    case TD_FUNCTION: return "Function";
    case TD_RANGE: return "Range";
    case TD_CHANNEL: return "Channel";
    case TD_BYTES: return "Bytes";
    case TD_MONEY: return "Money";
    case TD_ISOLATE: return "Isolate";
    case TD_CLASSVAL: return "Class";
    default: return d.cls->name->chars;  /* TD_CLASS */
    }
}

/* "str[]", "int32[]", "List": the name of a list holding `tag`. */
const char *list_type_name(int tag) { return LIST_NAMES[tag]; }

/* The name of a written type, for messages (a class by its name). */
const char *ann_name(const TypeAnn *t) {
    return t->d.kind == TD_CLASS ? lube_shown(t->cls) : desc_name(t->d);
}

int value_has_type(Value v, int tag) {
    switch (tag) {
    case TT_ANY: return 1;
    case TT_STRING: return IS_STRING(v);
    case TT_STROBJ: return IS_STROBJ(v);
    case TT_NUMBER: return IS_NUM(v);
    case TT_BOOL: return IS_BOOL(v);
    case TT_OBJECT: return IS_OBJ(v) && !IS_STRING(v);  /* every non-primitive is an Object */
    case TT_MAP: return IS_MAP(v) && AS_MAP(v)->dict && !AS_MAP(v)->klass;
    case TT_LIST: return IS_LIST(v);
    case TT_FUNCTION: return IS_CALLABLE(v);
    case TT_RANGE: return IS_RANGE(v);
    case TT_CHANNEL: return IS_CHANNEL(v);
    case TT_ISOLATE: return IS_ISOLATE(v);
    case TT_BYTES: return IS_BYTES(v);
    case TT_MONEY: return IS_MONEY(v);
    default: return IS_INT(v) && v.ikind == tag - TT_INT8;
    }
}

/* "a String", "an Object", "an int32": for error messages. A few rotating
 * buffers let one message use it several times. */
const char *a_type(Value v) { return with_article(type_name(v)); }

/* Only `false` is falsy. */
int is_truthy(Value v) { return !(IS_NIL(v) || (IS_BOOL(v) && !v.as.boolean)); }

/* Structural equality: lists and maps compare by contents. It walks with an
 * explicit work list, so deeply nested values cannot overflow the C stack. */
int values_equal(Value a, Value b) {
    Value small[64];
    Value *work = small;
    size_t n = 0, cap = 64;
    int equal = 1;
#define PUSH_PAIR(x, y)                                                     \
    do {                                                                    \
        if (n + 2 > cap) {                                                  \
            cap *= 2;                                                       \
            if (work == small) {                                            \
                work = xmalloc(cap * sizeof(Value));                        \
                memcpy(work, small, sizeof small);                          \
            } else {                                                        \
                work = xrealloc(work, cap * sizeof(Value));                 \
            }                                                               \
        }                                                                   \
        work[n++] = (x);                                                    \
        work[n++] = (y);                                                    \
    } while (0)

    PUSH_PAIR(a, b);
    while (n > 0 && equal) {
        Value y = work[--n], x = work[--n];
        if (IS_NUMERIC(x) && IS_NUMERIC(y)) {  /* by exact value across number types */
            equal = numeric_compare(x, y) == 0;
            continue;
        }
        if (IS_TEXT(x) && IS_TEXT(y)) {  /* a str and a String are equal when their text is */
            equal = text_equal(x, y);
            continue;
        }
        if (x.type != y.type) { equal = 0; break; }
        if (IS_BOOL(x)) {
            equal = x.as.boolean == y.as.boolean;
            continue;
        }
        /* otherwise both are objects: data never holds any other kind of value */
        if (x.as.obj == y.as.obj) continue;
        if (OBJ_TYPE(x) != OBJ_TYPE(y)) { equal = 0; break; }
        switch (OBJ_TYPE(x)) {
        case OBJ_LIST: {
            ObjList *p = AS_LIST(x), *q = AS_LIST(y);
            if (p->count != q->count) { equal = 0; break; }
            for (size_t i = 0; i < p->count; i++) PUSH_PAIR(p->items[i], q->items[i]);
            break;
        }
        case OBJ_MAP: {
            ObjMap *p = AS_MAP(x), *q = AS_MAP(y);
            if (p->count != q->count || p->klass != q->klass) { equal = 0; break; }
            for (size_t i = 0; i < p->used && equal; i++) {
                if (!p->entries[i].live) continue;
                Value other;
                if (!map_get(q, p->entries[i].key, &other)) equal = 0;
                else PUSH_PAIR(p->entries[i].value, other);
            }
            break;
        }
        case OBJ_RANGE: {
            ObjRange *p = AS_RANGE(x), *q = AS_RANGE(y);
            equal = p->start == q->start && p->end == q->end && p->step == q->step &&
                    p->inclusive == q->inclusive;
            break;
        }
        case OBJ_BYTES: {
            ObjBytes *p = AS_BYTES(x), *q = AS_BYTES(y);
            equal = p->len == q->len && (!p->len || !memcmp(p->data, q->data, p->len));
            break;
        }
        case OBJ_MONEY:
            equal = AS_MONEY(x)->units == AS_MONEY(y)->units && AS_MONEY(x)->cur == AS_MONEY(y)->cur;
            break;
        case OBJ_CHANNEL:  /* the same channel, held through two wrappers (an isolate's handle has just one) */
            equal = AS_CHANNEL(x)->ch == AS_CHANNEL(y)->ch;
            break;
        case OBJ_PACKAGE:  /* the same package, imported in two files */
            equal = AS_PACKAGE(x)->name == AS_PACKAGE(y)->name;
            break;
        default:
            equal = 0;  /* functions are equal only to themselves */
        }
    }
#undef PUSH_PAIR
    if (work != small) free(work);
    return equal;
}

/* The type's name, as written in Lube: "String", "Object", "int32[]", ... */
const char *type_name(Value v) {
    if (IS_BOOL(v)) return "Bool";
    if (IS_NUM(v)) return "Number";
    if (IS_INT(v)) return int_type_name(v.ikind);
    if (IS_SSTR(v)) return "str";
    if (!IS_OBJ(v)) return "nothing"; /* LCOV_EXCL_LINE: "no value" never reaches a type check */
    switch (OBJ_TYPE(v)) {
    case OBJ_STRING: return "str";
    case OBJ_STROBJ: return "String";
    case OBJ_LIST: return LIST_NAMES[AS_LIST(v)->elem];
    case OBJ_MAP:
        if (AS_MAP(v)->klass) return AS_MAP(v)->klass->name->chars;
        return AS_MAP(v)->dict ? map_name(AS_MAP(v)->ktype, AS_MAP(v)->vtype) : "Object";
    case OBJ_RANGE: return "Range";
    case OBJ_BYTES: return "Bytes";
    case OBJ_MONEY: return "Money";
    case OBJ_FOREIGN: return AS_FOREIGN(v)->klass->name->chars;
    case OBJ_CLASS: return "Class";
    case OBJ_CHANNEL: return "Channel";
    case OBJ_ISOLATE: return "Isolate";
    case OBJ_PACKAGE: return "Package";
    case OBJ_COMPONENT: return "Component";
    default: return "Function";  /* OBJ_FUNCTION or OBJ_NATIVE */
    }
}

/* ======================================================================
 * Printing
 * ==================================================================== */

/* A whole number under 1e16 either way, as its digits (exact in an int64; -0 is 0) at buf, which has room for
 * LUBE_WHOLE_ROOM bytes: how many digits (and sign) there are, with NULs after them to the end of the room; or
 * 0, with nothing written, for any other Number. Two digits at a time, from the last one: half the divisions,
 * and no digit to turn round. */
size_t format_whole(char *buf, double n) {
    static const char pairs[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
                                "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
                                "8081828384858687888990919293949596979899";
    if (!(n > -1e16 && n < 1e16)) return 0;  /* (nan too) */
    int64_t k = (int64_t)n;
    if ((double)k != n) return 0;
    uint64_t u = k < 0 ? 0 - (uint64_t)k : (uint64_t)k;
    char digits[2 * LUBE_WHOLE_ROOM] = {0}, *end = digits + LUBE_WHOLE_ROOM, *p = end;  /* (zeros: copied after the digits) */
    while (u >= 100) {
        uint64_t q = u / 100;
        memcpy(p -= 2, pairs + 2 * (u - q * 100), 2);
        u = q;
    }
    if (u >= 10) memcpy(p -= 2, pairs + 2 * u, 2);
    else *--p = (char)('0' + u);
    if (k < 0) *--p = '-';
    memcpy(buf, p, LUBE_WHOLE_ROOM);  /* (all of the room, whatever the length: two moves, no loop) */
    return (size_t)(end - p);
}

/* Integers print without a decimal point; other numbers use the shortest
 * representation that reads back as the same double. */
void format_number(char *buf, size_t size, double n) {
    char whole[LUBE_WHOLE_ROOM];
    size_t len;
    if (isnan(n)) {
        snprintf(buf, size, "nan");
    } else if (isinf(n)) {
        snprintf(buf, size, n > 0 ? "inf" : "-inf");
    } else if ((len = format_whole(whole, n)) != 0) {  /* a whole number: its digits */
        if (len >= size) len = size - 1;  /* (cut short, as snprintf would) */
        memcpy(buf, whole, len);
        buf[len] = '\0';
    } else {
        int64_t z = 0;
        int k = fabs(n) >= 1e-4 ? short_decimal(n, &z) : -1;
        if (k > 0 && size >= 24) {  /* (what %.15g shows for it, without it: the digits of z, a point k from the end) */
            char digits[24];
            int len = 0;
            uint64_t u = z < 0 ? 0 - (uint64_t)z : (uint64_t)z;
            do {
                digits[len++] = (char)('0' + u % 10);
                u /= 10;
            } while (u || len <= k);  /* (a 0 before the point, at least) */
            size_t i = 0;
            if (z < 0) buf[i++] = '-';
            while (len > 0) {
                buf[i++] = digits[--len];
                if (len == k) buf[i++] = '.';
            }
            buf[i] = '\0';
            return;
        }
        for (int prec = 15; prec <= 17; prec++) {
            snprintf(buf, size, "%.*g", prec, n);
            if (strtod(buf, NULL) == n) break;
        }
    }
}

static void append_quoted(StrBuf *sb, ObjString *s) {
    sb_putc(sb, '"');
    for (size_t i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->chars[i];
        switch (c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        default:
            if (c < 0x20 || c == 0x7f) sb_printf(sb, "\\x%02x", c);
            else sb_putc(sb, (char)c);
        }
    }
    sb_putc(sb, '"');
}

static int is_bare_key(ObjString *s) {
    if (s->length == 0) return 0;
    for (size_t i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->chars[i];
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
        int digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && i > 0))) return 0;
    }
    return 1;
}

#define MAX_PRINT_DEPTH 100

static void append_inner(StrBuf *sb, Value v, int quote, int depth) {
    char num[64];
    size_t n;
    StrTemp held;
    switch (v.type) {
    case VAL_BOOL: sb_puts(sb, v.as.boolean ? "true" : "false"); return;
    case VAL_NUM:
        n = format_whole(num, v.as.number);
        if (!n) {
            format_number(num, sizeof num, v.as.number);
            n = strlen(num);
        }
        sb_putn(sb, num, n);
        return;
    case VAL_INT:
        int_format(num, sizeof num, v);
        sb_puts(sb, num);
        return;
    case VAL_STR:
        if (quote) append_quoted(sb, text_in(v, &held));
        else sb_putn(sb, SSTR_CHARS(&v), SSTR_LEN(v));
        return;
    case VAL_OBJ: break;
    default: sb_puts(sb, "nothing"); return; /* LCOV_EXCL_LINE: "no value" is never printed */
    }

    switch (OBJ_TYPE(v)) {
    case OBJ_STRING:
        if (quote) append_quoted(sb, AS_STRING(v));
        else sb_putn(sb, AS_STRING(v)->chars, AS_STRING(v)->length);
        return;
    case OBJ_STROBJ:  /* prints as its text; quoted, it shows that it is a String */
        if (!quote) {
            sb_putn(sb, AS_STROBJ(v)->text->chars, AS_STROBJ(v)->text->length);
            return;
        }
        sb_puts(sb, "String(");
        append_quoted(sb, AS_STROBJ(v)->text);
        sb_putc(sb, ')');
        return;
    case OBJ_LIST: {
        ObjList *l = AS_LIST(v);
        if (depth >= MAX_PRINT_DEPTH) { sb_puts(sb, "[...]"); return; }
        sb_putc(sb, '[');
        for (size_t i = 0; i < l->count; i++) {
            if (i) sb_puts(sb, ", ");
            append_inner(sb, l->items[i], 1, depth + 1);
        }
        sb_putc(sb, ']');
        return;
    }
    case OBJ_CLASS:
        sb_printf(sb, "<class %s>", AS_CLASS(v)->name->chars);
        return;
    case OBJ_CHANNEL:
        sb_puts(sb, "<channel>");
        return;
    case OBJ_ISOLATE:
        sb_puts(sb, "<isolate>");
        return;
    case OBJ_PACKAGE:
        sb_printf(sb, "<package %s>", AS_PACKAGE(v)->name->chars);
        return;
    case OBJ_MAP: {
        ObjMap *m = AS_MAP(v);
        if (depth >= MAX_PRINT_DEPTH) { sb_puts(sb, "{...}"); return; }
        if (m->klass) sb_printf(sb, "%s ", m->klass->name->chars);  /* Dog {name: "rex"} */
        sb_putc(sb, '{');
        int first = 1;
        for (size_t i = 0; i < m->used; i++) {
            MapEntry *e = &m->entries[i];
            if (!e->live) continue;
            if (!first) sb_puts(sb, ", ");
            first = 0;
            StrTemp kt;
            ObjString *ks = IS_STRING(e->key) ? text_in(e->key, &kt) : NULL;
            if (ks && is_bare_key(ks))
                sb_putn(sb, ks->chars, ks->length);
            else
                append_inner(sb, e->key, 1, depth + 1);
            sb_puts(sb, ": ");
            append_inner(sb, e->value, 1, depth + 1);
        }
        sb_putc(sb, '}');
        return;
    }
    case OBJ_FUNCTION: {
        FuncDecl *d = AS_FUNCTION(v)->decl;
        if (d->name) sb_printf(sb, "<fn %s>", d->name->chars);
        else sb_puts(sb, "<fn>");
        return;
    }
    case OBJ_NATIVE:
        sb_printf(sb, AS_NATIVE(v)->cell ? "<handler %s>" : "<builtin %s>", AS_NATIVE(v)->name);
        return;
    case OBJ_COMPONENT:
        sb_printf(sb, "<component %s>", AS_COMPONENT(v)->decl->name->chars);
        return;
    case OBJ_BYTES: {  /* Bytes(68 69): in hex, the first 64 */
        ObjBytes *b = AS_BYTES(v);
        sb_puts(sb, "Bytes(");
        for (size_t i = 0; i < b->len && i < 64; i++) sb_printf(sb, i ? " %02x" : "%02x", b->data[i]);
        if (b->len > 64) sb_printf(sb, " ... %lu bytes", (unsigned long)b->len);
        sb_putc(sb, ')');
        return;
    }
    case OBJ_FOREIGN:  /* as its plugin says: Socket(tcp 127.0.0.1:5000) */
        lube_foreign_describe(sb, v);
        return;
    case OBJ_MONEY:  /* $1,234.50 */
        lube_money_describe(sb, v);
        return;
    case OBJ_RANGE: {
        ObjRange *r = AS_RANGE(v);
        char a[64], b[64], c[64];
        format_number(a, sizeof a, r->start);
        format_number(b, sizeof b, r->end);
        if (r->step == 1) {
            sb_printf(sb, "%s%s%s", a, r->inclusive ? "..=" : "..", b);
        } else {
            format_number(c, sizeof c, r->step);
            sb_printf(sb, "range(%s, %s, %s)", a, b, c);
        }
        return;
    }
    }
}

/* Appends the printed form of v. With quote=0 a top-level string is written
 * raw (as print() does); strings inside lists and maps are always quoted. */
void append_value(StrBuf *sb, Value v, int quote) { append_inner(sb, v, quote, 0); }

/* The string form of v, as str() returns it (a new reference). */
ObjString *value_to_string(Value v) {
    if (IS_TEXT(v)) return str_object(v);
    char small[128];
    if (v.type == VAL_NUM) {  /* (Numbers, the most common: no buffer at all) */
        size_t n = format_whole(small, v.as.number);
        if (!n) {
            format_number(small, sizeof small, v.as.number);
            n = strlen(small);
        }
        return new_string(small, n);
    }
    StrBuf sb;
    sb_init_on(&sb, small, sizeof small);
    append_value(&sb, v, 0);
    ObjString *s = new_string(sb.data, sb.len);
    sb_free(&sb);
    return s;
}

/* str(v) as a value: a short result is held in it, with no ObjString made at all. */
Value value_str(Value v) {
    if (IS_STRING(v)) return retain(v);
    if (IS_STROBJ(v)) return retain(OBJ_VAL(AS_STROBJ(v)->text));
    char small[128];
    if (v.type == VAL_NUM) {
        size_t n = format_whole(small, v.as.number);
        if (!n) {
            format_number(small, sizeof small, v.as.number);
            n = strlen(small);
        }
        return str_value(small, n);
    }
    StrBuf sb;
    sb_init_on(&sb, small, sizeof small);
    append_value(&sb, v, 0);
    Value r = str_value(sb.data, sb.len);
    sb_free(&sb);
    return r;
}

/* The quoted form of v, as the REPL shows it (a new reference). */
ObjString *value_repr(Value v) {
    char small[128];
    StrBuf sb;
    sb_init_on(&sb, small, sizeof small);
    append_value(&sb, v, 1);
    ObjString *s = new_string(sb.data, sb.len);
    sb_free(&sb);
    return s;
}
