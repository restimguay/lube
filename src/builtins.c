/*
 * builtins.c - Lube's standard library.
 *
 * Every builtin is a global function. Because `x.f(a)` calls `f(x, a)`, they
 * all work in method style too: "hi".upper(), nums.map(fn(n) => n * 2).
 *
 * Conventions for a native function:
 *   - argv is borrowed; the result is a new reference, or ERR_VAL after
 *     raising an error with lube_error().
 *   - A native marked "changes first" (push, pop, ...) receives its first
 *     argument lent with &: it may change argv[0] in place, after calling
 *     make_unique() so that nobody else's copy is affected.
 */
#include "lube.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NATIVE(name) static Value name(Interp *I, int argc, Value *argv)
#define FNAME (__func__ + 2)  /* "n_split" -> "split" */

static Value type_error(Interp *I, const char *fn, int i, const char *expected, Value got) {
    return lube_error(I, "%s() expects %s as argument %d, got %s", fn, expected, i + 1, type_name(got));
}

#define NEED(cond, i, what) \
    do { if (!(cond)) return type_error(I, FNAME, (i), (what), argv[i]); } while (0)
#define IS_WHOLE(v) (IS_NUMERIC(v) && numeric_value(v) == floor(numeric_value(v)))
#define NEED_NUM(i) NEED(IS_NUMERIC(argv[i]), i, "a Number")
#define NEED_INT(i) NEED(IS_WHOLE(argv[i]), i, "a whole Number")
#define NEED_STR(i) NEED(IS_TEXT(argv[i]), i, "a str")  /* a str or a String */
#define NEED_LIST(i) NEED(IS_LIST(argv[i]), i, "a List")
#define NEED_MAP(i) NEED(IS_MAP(argv[i]), i, "an Object")
#define NEED_FN(i) NEED(IS_CALLABLE(argv[i]), i, "a Function")

/* A function given to map(), filter() and friends must produce a value: a
 * function without one would put "nothing" into the result. Takes over r. */
static Value need_result(Interp *I, Value r, const char *fn) {
    if (!IS_NIL(r)) return r;
    return lube_error(I, "the function given to %s() did not return a value", fn);
}

/* Does this function want an index as its second argument? */
static int wants_index(Value f) { return IS_FUNCTION(f) && AS_FUNCTION(f)->decl->nparams >= 2; }

/* Appends the UTF-8 characters of s to out. */
static void push_chars(Interp *I, ObjList *out, ObjString *s) {
    size_t i = 0;
    while (i < s->length) {
        size_t len = utf8_seq_len((unsigned char)s->chars[i]);
        if (i + len > s->length) len = s->length - i;
        list_push(out, len == 1 ? retain(OBJ_VAL(I->chars[(unsigned char)s->chars[i]]))
                                : OBJ_VAL(new_string(s->chars + i, len)));
        i += len;
    }
}

/* Turns any iterable into a list (a new reference), or raises an error and
 * returns NULL. */
static ObjList *to_list(Interp *I, Value v, const char *fn) {
    if (IS_LIST(v)) return (ObjList *)retain(v).as.obj;
    if (IS_RANGE(v)) {
        ObjRange *r = AS_RANGE(v);
        double count = range_length(r);
        if (count > (double)LUBE_MAX_ITEMS) {
            lube_error(I, "%s(): the range has too many elements (%g) to make a list", fn, count);
            return NULL;
        }
        size_t n = (size_t)count;  /* safe: checked above */
        ObjList *out = new_list();
        list_reserve(out, n);
        for (size_t i = 0; i < n; i++) out->items[out->count++] = NUM_VAL(r->start + (double)i * r->step);
        return out;
    }
    if (IS_TEXT(v)) {
        ObjList *out = new_list();
        push_chars(I, out, text_of(v));
        return out;
    }
    if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        ObjList *out = new_list();
        list_reserve(out, m->count);
        for (size_t i = 0; i < m->used; i++)
            if (m->entries[i].live) out->items[out->count++] = retain(m->entries[i].key);
        return out;
    }
    lube_error(I, "%s() expects a List, Range, str or Object, got %s", fn, type_name(v));
    return NULL;
}

#define LIST_ARG(var, i) \
    ObjList *var = to_list(I, argv[i], FNAME); \
    if (!var) return ERR_VAL

/* -1, 0 or 1; *failed is set after raising an error for incomparable values. */
static int compare(Interp *I, Value a, Value b, const char *fn, int *failed) {
    if (IS_NUMERIC(a) && IS_NUMERIC(b)) {
        int c = numeric_compare(a, b);
        if (c != 2) return c;
        lube_error(I, "%s() cannot order nan", fn);
        *failed = 1;
        return 0;
    }
    if (IS_TEXT(a) && IS_TEXT(b)) {
        ObjString *x = text_of(a), *y = text_of(b);
        size_t n = x->length < y->length ? x->length : y->length;
        int c = memcmp(x->chars, y->chars, n);
        if (c) return c < 0 ? -1 : 1;
        return x->length < y->length ? -1 : x->length > y->length;
    }
    lube_error(I, "%s() cannot compare %s with %s", fn, a_type(a), a_type(b));
    *failed = 1;
    return 0;
}

static Value from_buf(StrBuf *sb) {
    ObjString *s = new_string(sb->data ? sb->data : "", sb->len);
    sb_free(sb);
    return OBJ_VAL(s);
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* ======================================================================
 * Input and output
 * ==================================================================== */

static void write_values(int argc, Value *argv, int newline) {
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < argc; i++) {
        if (i) sb_putc(&sb, ' ');
        append_value(&sb, argv[i], 0);
    }
    if (newline) sb_putc(&sb, '\n');
    if (sb.len) lube_output(sb.data, sb.len, 0);
    sb_free(&sb);
}

NATIVE(n_print) {
    (void)I;
    write_values(argc, argv, 1);
    return NIL_VAL;
}

NATIVE(n_write) {
    (void)I;
    write_values(argc, argv, 0);
    return NIL_VAL;
}

NATIVE(n_input) {
    (void)I;
    if (argc > 0) write_values(1, argv, 0);
    fflush(stdout);
    StrBuf sb;
    sb_init(&sb);
    int c, got = 0;
    while ((c = getchar()) != EOF) {
        got = 1;
        if (c == '\n') break;
        sb_putc(&sb, (char)c);
    }
    if (!got) {
        sb_free(&sb);
        return lube_error(I, "input(): end of input");
    }
    if (sb.len && sb.data[sb.len - 1] == '\r') sb.len--;
    return from_buf(&sb);
}

NATIVE(n_read_file) {
    (void)argc;
    NEED_STR(0);
    size_t len;
    char *text = read_file(text_of(argv[0])->chars, &len);
    if (!text) return lube_error(I, "read_file(): cannot read '%s'", text_of(argv[0])->chars);
    ObjString *s = new_string(text, len);
    free(text);
    return OBJ_VAL(s);
}

static Value write_out(Interp *I, Value *argv, const char *fn, const char *mode) {
    if (!IS_TEXT(argv[0])) return type_error(I, fn, 0, "a str", argv[0]);
    const char *path = text_of(argv[0])->chars;
    ObjString *content = value_to_string(argv[1]);
    FILE *f = fopen(path, mode);
    if (!f) {
        release(OBJ_VAL(content));
        return lube_error(I, "%s(): cannot open '%s' for writing", fn, path);
    }
    size_t written = fwrite(content->chars, 1, content->length, f);
    int failed = fclose(f) != 0 || written != content->length;
    release(OBJ_VAL(content));
    if (failed) return lube_error(I, "%s(): failed to write '%s'", fn, path);
    return NIL_VAL;
}

NATIVE(n_write_file) { (void)argc; return write_out(I, argv, "write_file", "wb"); }
NATIVE(n_append_file) { (void)argc; return write_out(I, argv, "append_file", "ab"); }

NATIVE(n_exit) {
    if (argc > 0) NEED(IS_WHOLE(argv[0]) && numeric_value(argv[0]) >= 0 && numeric_value(argv[0]) <= 255, 0,
                       "a whole number from 0 to 255");
    int code = argc > 0 ? (int)numeric_value(argv[0]) : 0;
    if (lube_hosted) {  /* inside a host program: back to it (lube_run gives LUBE_EXIT), not the end of it */
        I->exiting = 1;
        I->exit_code = code;
        return lube_error(I, "exit(%d)", code);
    }
    fflush(stdout);
    exit(code);
}

NATIVE(n_clock) { (void)I; (void)argc; (void)argv; return NUM_VAL((double)clock() / CLOCKS_PER_SEC); }
NATIVE(n_time) { (void)I; (void)argc; (void)argv; return NUM_VAL((double)time(NULL)); }

/* ======================================================================
 * Types and conversion
 * ==================================================================== */

NATIVE(n_type) {
    (void)argc;
    const char *t = type_name(argv[0]);
    return retain(OBJ_VAL(lube_intern(I, t, strlen(t))));
}

NATIVE(n_str) { (void)I; (void)argc; return OBJ_VAL(value_to_string(argv[0])); }
NATIVE(n_repr) { (void)I; (void)argc; return OBJ_VAL(value_repr(argv[0])); }

/* num(x) and int(x): text that is not a number is an error, unless a
 * fallback is given: num("abc", 0) is 0. */
static Value to_number(Interp *I, int argc, Value *argv, const char *fn) {
    Value v = argv[0];
    if (IS_NUM(v)) return v;
    if (IS_INT(v)) return NUM_VAL(numeric_value(v));
    if (IS_BOOL(v)) return NUM_VAL(v.as.boolean ? 1 : 0);
    if (IS_TEXT(v)) {
        const char *s = text_of(v)->chars;
        while (is_space(*s)) s++;
        char *end = (char *)s;
        double d = *s ? strtod(s, &end) : 0;
        int ok = end != s;
        while (ok && is_space(*end)) end++;
        if (ok && !*end) return NUM_VAL(d);
        if (argc > 1) return retain(argv[1]);
        return lube_error(I, "%s(): '%s' is not a number (give a fallback to use instead: %s(text, 0))", fn,
                          text_of(v)->chars, fn);
    }
    return lube_error(I, "%s() cannot convert %s to a Number", fn, a_type(v));
}

NATIVE(n_num) { return to_number(I, argc, argv, "num"); }

NATIVE(n_int) {
    Value v = to_number(I, argc, argv, "int");
    if (!IS_NUM(v)) return v;
    double d = v.as.number;
    return NUM_VAL(d < 0 ? ceil(d) : floor(d));
}

NATIVE(n_bool) { (void)I; (void)argc; return BOOL_VAL(is_truthy(argv[0])); }

/* ======================================================================
 * Collections
 * ==================================================================== */

NATIVE(n_len) {
    (void)argc;
    Value v = argv[0];
    if (IS_TEXT(v)) return NUM_VAL((double)text_of(v)->length);
    if (IS_LIST(v)) return NUM_VAL((double)AS_LIST(v)->count);
    if (IS_MAP(v)) return NUM_VAL((double)AS_MAP(v)->count);
    if (IS_RANGE(v)) return NUM_VAL(range_length(AS_RANGE(v)));
    if (IS_BYTES(v)) return NUM_VAL((double)AS_BYTES(v)->len);
    return type_error(I, "len", 0, "a str, List, Object, Range or Bytes", v);
}

/* --- functions that change their first argument (taken with &) --- */

NATIVE(n_push) {
    if (IS_BYTES(argv[0])) return lube_bytes_push(I, argc, argv);
    NEED_LIST(0);
    int elem = AS_LIST(argv[0])->elem;
    for (int i = 1; i < argc && elem != TT_ANY; i++) {  /* check everything before changing anything */
        Value c = lube_check_elem(I, type_name(argv[0]), elem, argv[i]);
        if (IS_ERR(c)) return c;
        release(c);
    }
    make_unique(&argv[0]);
    ObjList *l = AS_LIST(argv[0]);
    for (int i = 1; i < argc; i++) list_push(l, lube_check_elem(I, type_name(argv[0]), elem, argv[i]));  /* checked */
    return NIL_VAL;
}

NATIVE(n_pop) {
    (void)argc;
    NEED_LIST(0);
    if (AS_LIST(argv[0])->count == 0) return lube_error(I, "pop() called on an empty list");
    make_unique(&argv[0]);
    ObjList *l = AS_LIST(argv[0]);
    return l->items[--l->count];  /* the reference moves to the caller */
}

NATIVE(n_insert) {
    (void)argc;
    NEED_LIST(0);
    NEED_INT(1);
    Value item = lube_check_elem(I, type_name(argv[0]), AS_LIST(argv[0])->elem, argv[2]);
    if (IS_ERR(item)) return item;
    make_unique(&argv[0]);
    ObjList *l = AS_LIST(argv[0]);
    double idx = numeric_value(argv[1]);
    double pos = idx < 0 ? idx + (double)l->count : idx;
    if (pos < 0) pos = 0;
    if (pos > (double)l->count) pos = (double)l->count;
    size_t p = (size_t)pos;
    list_reserve(l, l->count + 1);
    memmove(l->items + p + 1, l->items + p, (l->count - p) * sizeof(Value));
    l->items[p] = item;
    l->count++;
    return NIL_VAL;
}

NATIVE(n_remove) {
    (void)argc;
    NEED_LIST(0);
    NEED_INT(1);
    ObjList *l = AS_LIST(argv[0]);
    double idx = numeric_value(argv[1]);
    double pos = idx < 0 ? idx + (double)l->count : idx;
    if (pos < 0 || pos >= (double)l->count)
        return lube_error(I, "remove(): index %g is out of range (length %lu)", idx, (unsigned long)l->count);
    make_unique(&argv[0]);
    l = AS_LIST(argv[0]);
    size_t p = (size_t)pos;
    Value v = l->items[p];
    memmove(l->items + p, l->items + p + 1, (l->count - p - 1) * sizeof(Value));
    l->count--;
    return v;
}

/* Removes a key; returns whether it was there. */
/* An instance always has exactly the fields its class declares. */
static Value fixed_fields(Interp *I, const char *fn, Value v) {
    return lube_error(I, "%s() cannot remove fields from %s: an instance always has every field its class declares", fn,
                      a_type(v));
}

NATIVE(n_delete) {
    (void)argc;
    NEED_MAP(0);
    if (IS_INSTANCE(argv[0])) return fixed_fields(I, FNAME, argv[0]);
    if (!map_key_ok(AS_MAP(argv[0]), argv[1]) || !map_get(AS_MAP(argv[0]), argv[1], NULL)) return BOOL_VAL(0);
    make_unique(&argv[0]);
    map_delete(AS_MAP(argv[0]), argv[1], NULL);
    return BOOL_VAL(1);
}

NATIVE(n_clear) {
    (void)argc;
    NEED(IS_LIST(argv[0]) || IS_MAP(argv[0]), 0, "a List or Object");
    if (IS_INSTANCE(argv[0])) return fixed_fields(I, FNAME, argv[0]);
    Value empty;
    if (IS_LIST(argv[0])) {  /* a cleared String[] is still a String[] */
        ObjList *l = new_list();
        l->elem = AS_LIST(argv[0])->elem;
        empty = OBJ_VAL(l);
    } else {  /* a cleared Map[K, V] is still one */
        ObjMap *src = AS_MAP(argv[0]), *m = new_map();
        m->dict = src->dict;
        m->ktype = src->ktype;
        m->vtype = src->vtype;
        empty = OBJ_VAL(m);
    }
    release(argv[0]);
    argv[0] = empty;
    return NIL_VAL;
}

/* --- functions that return new values --- */

static size_t clamp_index(double d, size_t len) {
    if (d != d) return 0;  /* NaN */
    if (d < 0) d += (double)len;
    if (d < 0) return 0;
    if (d > (double)len) return len;
    return (size_t)d;
}

NATIVE(n_slice) {
    Value v = argv[0];
    NEED(IS_LIST(v) || IS_TEXT(v), 0, "a List or String");
    if (argc > 1 && !IS_NIL(argv[1])) NEED_NUM(1);
    if (argc > 2 && !IS_NIL(argv[2])) NEED_NUM(2);
    size_t len = IS_LIST(v) ? AS_LIST(v)->count : text_of(v)->length;
    size_t start = argc > 1 && !IS_NIL(argv[1]) ? clamp_index(floor(numeric_value(argv[1])), len) : 0;
    size_t end = argc > 2 && !IS_NIL(argv[2]) ? clamp_index(floor(numeric_value(argv[2])), len) : len;
    if (end < start) end = start;
    if (IS_TEXT(v)) return OBJ_VAL(new_string(text_of(v)->chars + start, end - start));
    ObjList *src = AS_LIST(v), *out = new_list();
    out->elem = src->elem;
    list_reserve(out, end - start);
    for (size_t i = start; i < end; i++) out->items[out->count++] = retain(src->items[i]);
    return OBJ_VAL(out);
}

NATIVE(n_reverse) {
    (void)argc;
    if (IS_TEXT(argv[0])) {
        ObjString *s = text_of(argv[0]);
        ObjString *r = alloc_string(s->length);
        for (size_t i = 0; i < s->length; i++) r->chars[i] = s->chars[s->length - 1 - i];
        return OBJ_VAL(r);
    }
    LIST_ARG(src, 0);
    ObjList *out = new_list();
    out->elem = src->elem;
    list_reserve(out, src->count);
    for (size_t i = src->count; i > 0; i--) out->items[out->count++] = retain(src->items[i - 1]);
    release(OBJ_VAL(src));
    return OBJ_VAL(out);
}

/* Is b < a? (1 = yes, 0 = no, -1 = error) */
static int sort_before(Interp *I, Value cmp, Value b, Value a) {
    if (IS_NIL(cmp)) {
        int failed = 0;
        int c = compare(I, b, a, "sort", &failed);
        return failed ? -1 : c < 0;
    }
    Value args[2] = {b, a};
    Value r = need_result(I, lube_call(I, cmp, 2, args), "sort");
    if (IS_ERR(r)) return -1;
    int before;
    if (IS_BOOL(r)) before = r.as.boolean;
    else if (IS_NUMERIC(r)) before = numeric_value(r) < 0;
    else {
        lube_error(I, "sort() comparator must return a Number or a Bool, not %s", a_type(r));
        release(r);
        return -1;
    }
    release(r);
    return before;
}

/* sort() of plain Numbers: the same merge sort and the same comparisons
 * (b < a, and nan cannot be ordered) on the doubles themselves. Fills out;
 * 0, or -1 after an error. */
/* The n Numbers in a, in order, by their bits (a stable radix sort: what
 * the merge sort below makes, faster on many). Each one's key orders like
 * the Number; -0 has 0's, and the zeros get back their signs in their first
 * order (as a stable sort leaves them). 0 when there is a nan. */
static int radix_numbers(double *a, double *tmp, size_t n) {
    uint64_t *k = (uint64_t *)(void *)a, *t = (uint64_t *)(void *)tmp;
    unsigned char *signs = NULL;
    size_t zeros = 0, (*count)[256] = xmalloc(8 * sizeof *count);
    memset(count, 0, 8 * sizeof *count);
    for (size_t i = 0; i < n; i++) {
        double x = a[i];
        if (x != x) {
            free(count);
            return 0;
        }
        if (x == 0) {  /* (its sign, kept for later) */
            if (!signs) signs = xmalloc(n - i);
            signs[zeros++] = signbit(x) != 0;
            x = 0;
        }
        uint64_t b;
        memcpy(&b, &x, sizeof b);
        k[i] = b >> 63 ? ~b : b | 0x8000000000000000ull;  /* (negatives: reversed) */
        for (int d = 0; d < 8; d++) count[d][(k[i] >> (8 * d)) & 255]++;
    }
    for (int d = 0; d < 8; d++) {
        if (count[d][(k[0] >> (8 * d)) & 255] == n) continue;  /* (every key has this byte) */
        size_t at = 0;
        for (int b = 0; b < 256; b++) {
            size_t c = count[d][b];
            count[d][b] = at;
            at += c;
        }
        for (size_t i = 0; i < n; i++) t[count[d][(k[i] >> (8 * d)) & 255]++] = k[i];
        uint64_t *swap = k;
        k = t;
        t = swap;
    }
    size_t z = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t b = k[i] >> 63 ? k[i] & 0x7fffffffffffffffull : ~k[i];
        memcpy(&a[i], &b, sizeof b);  /* (a may be where k is: same place, same size) */
        if (a[i] == 0 && signs[z++]) a[i] = -0.0;
    }
    free(signs);
    free(count);
    return 1;
}

static int sort_numbers(Interp *I, ObjList *src, ObjList *out) {
    size_t n = src->count;
    double *a = xmalloc(sizeof(double) * (n ? n : 1)), *tmp = xmalloc(sizeof(double) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) a[i] = src->items[i].as.number;
    int failed = 0;
    if (n < 64 || !radix_numbers(a, tmp, n)) {  /* few, or a nan (and its error): a merge sort */
        for (size_t i = 0; i < n; i++) a[i] = src->items[i].as.number;
        for (size_t width = 1; width < n && !failed; width *= 2) {
            for (size_t lo = 0; lo < n && !failed; lo += 2 * width) {
                size_t mid = lo + width < n ? lo + width : n;
                size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
                size_t i = lo, j = mid, k = lo;
                while (i < mid && j < hi) {
                    if (a[j] != a[j] || a[i] != a[i]) {
                        lube_error(I, "sort() cannot order nan");
                        failed = 1;
                        break;
                    }
                    tmp[k++] = a[j] < a[i] ? a[j++] : a[i++];
                }
                while (i < mid) tmp[k++] = a[i++];
                while (j < hi) tmp[k++] = a[j++];
            }
            double *t = a;
            a = tmp;
            tmp = t;
        }
    }
    if (!failed) {
        list_reserve(out, n);
        for (size_t i = 0; i < n; i++) out->items[out->count++] = NUM_VAL(a[i]);
    }
    free(a);
    free(tmp);
    return failed ? -1 : 0;
}

/* Stable bottom-up merge sort into a new list. */
NATIVE(n_sort) {
    Value cmp = argc > 1 ? argv[1] : NIL_VAL;
    if (!IS_NIL(cmp)) NEED_FN(1);
    LIST_ARG(src, 0);
    size_t n = src->count, numbers = IS_NIL(cmp);
    for (size_t i = 0; i < n && numbers; i++) numbers = IS_NUM(src->items[i]);
    if (numbers) {  /* the common case, without a comparison call for each pair */
        ObjList *out = new_list();
        out->elem = src->elem;
        Value result = sort_numbers(I, src, out) < 0 ? ERR_VAL : OBJ_VAL(out);
        if (IS_ERR(result)) release(OBJ_VAL(out));
        release(OBJ_VAL(src));
        return result;
    }
    Value *a = xmalloc(sizeof(Value) * (n ? n : 1));
    Value *tmp = xmalloc(sizeof(Value) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) a[i] = src->items[i];  /* borrowed from src */
    int failed = 0;
    for (size_t width = 1; width < n && !failed; width *= 2) {
        for (size_t lo = 0; lo < n && !failed; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int before = sort_before(I, cmp, a[j], a[i]);
                if (before < 0) { failed = 1; break; }
                tmp[k++] = before ? a[j++] : a[i++];
            }
            if (failed) break;
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi) tmp[k++] = a[j++];
        }
        Value *t = a;
        a = tmp;
        tmp = t;
    }
    Value result = ERR_VAL;
    if (!failed) {
        ObjList *out = new_list();
        out->elem = src->elem;
        list_reserve(out, n);
        for (size_t i = 0; i < n; i++) out->items[out->count++] = retain(a[i]);
        result = OBJ_VAL(out);
    }
    free(a);
    free(tmp);
    release(OBJ_VAL(src));
    return result;
}

NATIVE(n_contains) {
    (void)argc;
    int r = lube_contains(I, argv[0], argv[1]);
    return r < 0 ? ERR_VAL : BOOL_VAL(r);
}

NATIVE(n_index_of) {
    (void)argc;
    if (IS_TEXT(argv[0])) {
        NEED_STR(1);
        ObjString *s = text_of(argv[0]), *sub = text_of(argv[1]);
        return NUM_VAL((double)str_find(s->chars, s->length, sub->chars, sub->length, 0));
    }
    NEED_LIST(0);
    ObjList *l = AS_LIST(argv[0]);
    for (size_t i = 0; i < l->count; i++)
        if (values_equal(l->items[i], argv[1])) return NUM_VAL((double)i);
    return NUM_VAL(-1);
}

NATIVE(n_join) {
    if (IS_ISOLATE(argv[0])) {  /* iso.join(): waits for an isolate (isolate.c) */
        if (argc > 1) return lube_error(I, "join() takes just the isolate when waiting for one");
        return lube_join_isolate(I, argv[0]);
    }
    if (argc > 1) NEED_STR(1);
    LIST_ARG(l, 0);
    ObjString *sep = argc > 1 ? text_of(argv[1]) : NULL;
    size_t total = 0, i = 0;
    for (; i < l->count && IS_TEXT(l->items[i]) && total <= LUBE_MAX_BYTES; i++)  /* only text: its length first */
        total += text_of(l->items[i])->length + (i && sep ? sep->length : 0);
    if (i == l->count && total <= LUBE_MAX_BYTES) {
        ObjString *out = alloc_string(total);
        char *at = out->chars;
        for (i = 0; i < l->count; i++) {
            ObjString *t = text_of(l->items[i]);
            if (i && sep) {
                memcpy(at, sep->chars, sep->length);
                at += sep->length;
            }
            memcpy(at, t->chars, t->length);
            at += t->length;
        }
        release(OBJ_VAL(l));
        return OBJ_VAL(out);
    }
    StrBuf sb;
    sb_init(&sb);
    for (i = 0; i < l->count; i++) {
        if (i && sep) sb_putn(&sb, sep->chars, sep->length);
        append_value(&sb, l->items[i], 0);
    }
    release(OBJ_VAL(l));
    return from_buf(&sb);
}

/* keys() and values() of a Map[K, V] are K[] and V[] lists. */
NATIVE(n_keys) {
    (void)argc;
    NEED_MAP(0);
    ObjMap *m = AS_MAP(argv[0]);
    ObjList *out = new_list();
    out->elem = m->ktype;
    list_reserve(out, m->count);
    for (size_t i = 0; i < m->used; i++)
        if (m->entries[i].live) out->items[out->count++] = retain(m->entries[i].key);
    return OBJ_VAL(out);
}

NATIVE(n_values) {
    (void)argc;
    NEED_MAP(0);
    ObjMap *m = AS_MAP(argv[0]);
    ObjList *out = new_list();
    out->elem = m->vtype;
    list_reserve(out, m->count);
    for (size_t i = 0; i < m->used; i++)
        if (m->entries[i].live) out->items[out->count++] = retain(m->entries[i].value);
    return OBJ_VAL(out);
}

static ObjList *pair(Value a, Value b) {
    ObjList *p = new_list();
    list_push(p, retain(a));
    list_push(p, retain(b));
    return p;
}

NATIVE(n_items) {
    (void)argc;
    NEED_MAP(0);
    ObjMap *m = AS_MAP(argv[0]);
    ObjList *out = new_list();
    for (size_t i = 0; i < m->used; i++)
        if (m->entries[i].live) list_push(out, OBJ_VAL(pair(m->entries[i].key, m->entries[i].value)));
    return OBJ_VAL(out);
}

NATIVE(n_has) {
    (void)argc;
    NEED_MAP(0);
    return BOOL_VAL(map_key_ok(AS_MAP(argv[0]), argv[1]) && map_get(AS_MAP(argv[0]), argv[1], NULL));
}

NATIVE(n_get) {
    NEED_MAP(0);
    Value v;
    if (map_key_ok(AS_MAP(argv[0]), argv[1]) && map_get(AS_MAP(argv[0]), argv[1], &v)) return retain(v);
    if (argc > 2) return retain(argv[2]);
    ObjString *k = value_repr(argv[1]);
    Value e = lube_error(I, "get(): the object has no key %s (give a default: get(object, key, default))", k->chars);
    release(OBJ_VAL(k));
    return e;
}

NATIVE(n_list) {
    if (argc == 0) return OBJ_VAL(new_list());
    ObjList *l = to_list(I, argv[0], "list");
    return l ? OBJ_VAL(l) : ERR_VAL;
}

NATIVE(n_range) {
    for (int i = 0; i < argc; i++) NEED_NUM(i);
    double start = 0, end = numeric_value(argv[0]), step = 1;
    if (argc > 1) {
        start = numeric_value(argv[0]);
        end = numeric_value(argv[1]);
    }
    if (argc > 2) step = numeric_value(argv[2]);
    if (step == 0) return lube_error(I, "range() step cannot be zero");
    return OBJ_VAL(new_range(start, end, step, 0));
}

/* ---------- higher-order functions ---------- */

/* Calls f(item) or f(item, index), depending on how many parameters f has. */
static Value call_item(Interp *I, Value f, int idx, Value item, size_t i) {
    Value args[2] = {item, NUM_VAL((double)i)};
    return lube_call(I, f, idx ? 2 : 1, args);
}

NATIVE(n_map) {
    (void)argc;
    NEED_FN(1);
    LIST_ARG(src, 0);
    int idx = wants_index(argv[1]);
    ObjList *out = new_list();
    list_reserve(out, src->count);
    for (size_t i = 0; i < src->count; i++) {
        Value r = need_result(I, call_item(I, argv[1], idx, src->items[i], i), "map");
        if (IS_ERR(r)) {
            release(OBJ_VAL(out));
            release(OBJ_VAL(src));
            return r;
        }
        list_push(out, r);
    }
    release(OBJ_VAL(src));
    return OBJ_VAL(out);
}

NATIVE(n_filter) {
    (void)argc;
    NEED_FN(1);
    LIST_ARG(src, 0);
    int idx = wants_index(argv[1]);
    ObjList *out = new_list();
    out->elem = src->elem;
    for (size_t i = 0; i < src->count; i++) {
        Value r = need_result(I, call_item(I, argv[1], idx, src->items[i], i), "filter");
        if (IS_ERR(r)) {
            release(OBJ_VAL(out));
            release(OBJ_VAL(src));
            return r;
        }
        if (is_truthy(r)) list_push(out, retain(src->items[i]));
        release(r);
    }
    release(OBJ_VAL(src));
    return OBJ_VAL(out);
}

NATIVE(n_each) {
    (void)argc;
    NEED_FN(1);
    LIST_ARG(src, 0);
    int idx = wants_index(argv[1]);
    for (size_t i = 0; i < src->count; i++) {
        Value r = call_item(I, argv[1], idx, src->items[i], i);
        if (IS_ERR(r)) {
            release(OBJ_VAL(src));
            return r;
        }
        release(r);
    }
    release(OBJ_VAL(src));
    return NIL_VAL;
}

NATIVE(n_reduce) {
    NEED_FN(1);
    LIST_ARG(src, 0);
    size_t start = 0;
    Value acc;
    if (argc > 2) {
        acc = retain(argv[2]);
    } else {
        if (src->count == 0) {
            release(OBJ_VAL(src));
            return lube_error(I, "reduce() of an empty list needs a starting value");
        }
        acc = retain(src->items[0]);
        start = 1;
    }
    for (size_t i = start; i < src->count; i++) {
        Value args[2] = {acc, src->items[i]};
        Value r = need_result(I, lube_call(I, argv[1], 2, args), "reduce");
        release(acc);
        if (IS_ERR(r)) {
            release(OBJ_VAL(src));
            return r;
        }
        acc = r;
    }
    release(OBJ_VAL(src));
    return acc;
}

/* find(xs, f) is the first item where f is true; with no match it is an
 * error, unless a default is given: find(xs, f, default). */
static Value find_where(Interp *I, int argc, Value *argv, const char *fn, int want_index) {
    if (!IS_CALLABLE(argv[1])) return type_error(I, fn, 1, "a Function", argv[1]);
    ObjList *src = to_list(I, argv[0], fn);
    if (!src) return ERR_VAL;
    Value result = want_index ? NUM_VAL(-1) : argc > 2 ? retain(argv[2]) : NIL_VAL;
    for (size_t i = 0; i < src->count; i++) {
        Value r = need_result(I, lube_call(I, argv[1], 1, &src->items[i]), fn);
        if (IS_ERR(r)) {
            release(result);
            result = r;
            break;
        }
        int hit = is_truthy(r);
        release(r);
        if (hit) {
            release(result);
            result = want_index ? NUM_VAL((double)i) : retain(src->items[i]);
            break;
        }
    }
    release(OBJ_VAL(src));
    if (IS_NIL(result)) return lube_error(I, "find(): no item matched (give a default: find(xs, f, default))");
    return result;
}

NATIVE(n_find) { return find_where(I, argc, argv, "find", 0); }
NATIVE(n_find_index) { return find_where(I, argc, argv, "find_index", 1); }

static Value any_all(Interp *I, int argc, Value *argv, const char *fn, int want_all) {
    if (argc > 1 && !IS_CALLABLE(argv[1])) return type_error(I, fn, 1, "a Function", argv[1]);
    ObjList *src = to_list(I, argv[0], fn);
    if (!src) return ERR_VAL;
    Value result = BOOL_VAL(want_all);
    for (size_t i = 0; i < src->count; i++) {
        int t;
        if (argc > 1) {
            Value r = need_result(I, lube_call(I, argv[1], 1, &src->items[i]), fn);
            if (IS_ERR(r)) {
                result = r;
                break;
            }
            t = is_truthy(r);
            release(r);
        } else {
            t = is_truthy(src->items[i]);
        }
        if (want_all != t) {
            result = BOOL_VAL(t);
            break;
        }
    }
    release(OBJ_VAL(src));
    return result;
}

NATIVE(n_any) { return any_all(I, argc, argv, "any", 0); }
NATIVE(n_all) { return any_all(I, argc, argv, "all", 1); }

/* Adds with the same rules as `+`: sized integers stay checked. */
NATIVE(n_sum) {
    (void)argc;
    LIST_ARG(src, 0);
    Value total = NUM_VAL(0);
    for (size_t i = 0; i < src->count; i++) {
        Value item = src->items[i];
        if (!IS_NUMERIC(item)) {
            Value e = lube_error(I, "sum() can only add numbers, found %s", a_type(item));
            release(OBJ_VAL(src));
            return e;
        }
        total = i == 0 ? item : lube_arith(I, T_PLUS, total, item);
        if (IS_ERR(total)) break;
    }
    release(OBJ_VAL(src));
    return total;
}

static Value min_max(Interp *I, int argc, Value *argv, const char *fn, int want_max) {
    ObjList *l = NULL;
    Value *items = argv;
    size_t n = (size_t)argc;
    if (argc == 1) {
        l = to_list(I, argv[0], fn);
        if (!l) return ERR_VAL;
        items = l->items;
        n = l->count;
    }
    Value result;
    if (n == 0) {
        result = lube_error(I, "%s() of an empty list", fn);
    } else {
        Value best = items[0];
        int failed = 0;
        for (size_t i = 1; i < n && !failed; i++) {
            int c = compare(I, items[i], best, fn, &failed);
            if (want_max ? c > 0 : c < 0) best = items[i];
        }
        result = failed ? ERR_VAL : retain(best);
    }
    if (l) release(OBJ_VAL(l));
    return result;
}

NATIVE(n_min) { return min_max(I, argc, argv, "min", 0); }
NATIVE(n_max) { return min_max(I, argc, argv, "max", 1); }

NATIVE(n_zip) {
    (void)argc;
    LIST_ARG(a, 0);
    ObjList *b = to_list(I, argv[1], "zip");
    if (!b) {
        release(OBJ_VAL(a));
        return ERR_VAL;
    }
    ObjList *out = new_list();
    size_t n = a->count < b->count ? a->count : b->count;
    for (size_t i = 0; i < n; i++) list_push(out, OBJ_VAL(pair(a->items[i], b->items[i])));
    release(OBJ_VAL(a));
    release(OBJ_VAL(b));
    return OBJ_VAL(out);
}

/* ======================================================================
 * Strings
 * ==================================================================== */

static ObjString *change_case(ObjString *src, int upper) {
    ObjString *r = alloc_string(src->length);
    for (size_t i = 0; i < src->length; i++) {
        char c = src->chars[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        r->chars[i] = c;
    }
    return r;
}

NATIVE(n_upper) { (void)argc; NEED_STR(0); return OBJ_VAL(change_case(text_of(argv[0]), 1)); }
NATIVE(n_lower) { (void)argc; NEED_STR(0); return OBJ_VAL(change_case(text_of(argv[0]), 0)); }

/* Splits text into words at runs of whitespace, as a str[] list. */
static ObjList *words_of(ObjString *s) {
    ObjList *out = new_list();
    out->elem = TT_STRING;
    size_t i = 0;
    while (i < s->length) {
        while (i < s->length && is_space(s->chars[i])) i++;
        if (i >= s->length) break;
        size_t j = i;
        while (j < s->length && !is_space(s->chars[j])) j++;
        list_push(out, OBJ_VAL(new_string(s->chars + i, j - i)));
        i = j;
    }
    return out;
}

/* The read-only properties of a String object. */
Value lube_string_property(Interp *I, Value s, ObjString *name) {
    ObjString *t = AS_STROBJ(s)->text;
    const char *p = name->chars;
    if (!strcmp(p, "value")) return retain(OBJ_VAL(t));
    if (!strcmp(p, "upper")) return OBJ_VAL(change_case(t, 1));
    if (!strcmp(p, "lower")) return OBJ_VAL(change_case(t, 0));
    if (!strcmp(p, "words")) return OBJ_VAL(words_of(t));
    if (!strcmp(p, "chars")) {
        ObjList *out = new_list();
        out->elem = TT_STRING;
        push_chars(I, out, t);
        return OBJ_VAL(out);
    }
    if (!strcmp(p, "length")) {  /* in characters (UTF-8 aware); len() counts bytes */
        double n = 0;
        for (size_t i = 0; i < t->length; i += utf8_seq_len((unsigned char)t->chars[i])) n++;
        return NUM_VAL(n);
    }
    return lube_error(I, "a String has no property '%s' (it has length, value, upper, lower, chars and words)", p);
}

NATIVE(n_trim) {
    (void)argc;
    NEED_STR(0);
    ObjString *s = text_of(argv[0]);
    size_t a = 0, b = s->length;
    while (a < b && is_space(s->chars[a])) a++;
    while (b > a && is_space(s->chars[b - 1])) b--;
    return OBJ_VAL(new_string(s->chars + a, b - a));
}

NATIVE(n_split) {
    NEED_STR(0);
    if (argc > 1) NEED_STR(1);
    ObjString *s = text_of(argv[0]);
    ObjList *out = new_list();
    if (argc < 2) {  /* split on runs of whitespace */
        release(OBJ_VAL(out));
        return OBJ_VAL(words_of(s));
    } else {
        ObjString *sep = text_of(argv[1]);
        if (sep->length == 0) {
            push_chars(I, out, s);
        } else {
            size_t start = 0, pieces = 1;
            long pos;
            while ((pos = str_find(s->chars, s->length, sep->chars, sep->length, start)) >= 0) {  /* (how many first) */
                pieces++;
                start = (size_t)pos + sep->length;
            }
            list_reserve(out, pieces);
            start = 0;
            while ((pos = str_find(s->chars, s->length, sep->chars, sep->length, start)) >= 0) {
                list_push(out, OBJ_VAL(new_string(s->chars + start, (size_t)pos - start)));
                start = (size_t)pos + sep->length;
            }
            list_push(out, OBJ_VAL(new_string(s->chars + start, s->length - start)));
        }
    }
    return OBJ_VAL(out);
}

NATIVE(n_replace) {
    (void)argc;
    NEED_STR(0);
    NEED_STR(1);
    NEED_STR(2);
    ObjString *s = text_of(argv[0]), *old = text_of(argv[1]), *rep = text_of(argv[2]);
    if (old->length == 0) return retain(argv[0]);
    StrBuf sb;
    sb_init(&sb);
    size_t start = 0;
    long pos;
    while ((pos = str_find(s->chars, s->length, old->chars, old->length, start)) >= 0) {
        sb_putn(&sb, s->chars + start, (size_t)pos - start);
        sb_putn(&sb, rep->chars, rep->length);
        start = (size_t)pos + old->length;
    }
    sb_putn(&sb, s->chars + start, s->length - start);
    return from_buf(&sb);
}

NATIVE(n_starts_with) {
    (void)argc;
    NEED_STR(0);
    NEED_STR(1);
    ObjString *s = text_of(argv[0]), *p = text_of(argv[1]);
    return BOOL_VAL(p->length <= s->length && memcmp(s->chars, p->chars, p->length) == 0);
}

NATIVE(n_ends_with) {
    (void)argc;
    NEED_STR(0);
    NEED_STR(1);
    ObjString *s = text_of(argv[0]), *p = text_of(argv[1]);
    return BOOL_VAL(p->length <= s->length && memcmp(s->chars + s->length - p->length, p->chars, p->length) == 0);
}

NATIVE(n_chars) {
    (void)argc;
    NEED_STR(0);
    ObjList *out = new_list();
    push_chars(I, out, text_of(argv[0]));
    return OBJ_VAL(out);
}

NATIVE(n_ord) {
    (void)argc;
    NEED_STR(0);
    ObjString *s = text_of(argv[0]);
    if (s->length == 0) return lube_error(I, "ord() needs a non-empty string");
    const unsigned char *p = (const unsigned char *)s->chars;
    size_t len = utf8_seq_len(p[0]);
    if (len > s->length) len = 1;
    unsigned long cp = len == 1 ? p[0] : p[0] & (0xFFu >> (len + 1));
    for (size_t i = 1; i < len; i++) cp = (cp << 6) | (p[i] & 0x3F);
    return NUM_VAL((double)cp);
}

NATIVE(n_chr) {
    (void)argc;
    NEED_INT(0);
    double d = numeric_value(argv[0]);
    if (d < 0 || d > 0x10FFFF) return lube_error(I, "chr(): %g is not a valid character code", d);
    unsigned long cp = (unsigned long)d;
    char buf[4];
    size_t n;
    if (cp < 0x80) return retain(OBJ_VAL(I->chars[cp]));
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return OBJ_VAL(new_string(buf, n));
}

static Value pad(Interp *I, int argc, Value *argv, const char *fn, int left) {
    if (!IS_WHOLE(argv[1])) return type_error(I, fn, 1, "a whole Number", argv[1]);
    char fill = ' ';
    if (argc > 2) {
        if (!IS_TEXT(argv[2]) || text_of(argv[2])->length != 1)
            return lube_error(I, "%s(): the fill must be a single character", fn);
        fill = text_of(argv[2])->chars[0];
    }
    ObjString *s = value_to_string(argv[0]);
    double width = numeric_value(argv[1]);
    if (width <= (double)s->length) return OBJ_VAL(s);
    if (width > 1e9) {
        release(OBJ_VAL(s));
        return lube_error(I, "%s(): width is too large", fn);
    }
    size_t w = (size_t)width, extra = w - s->length;
    ObjString *out = alloc_string(w);
    if (left) {
        memset(out->chars, fill, extra);
        memcpy(out->chars + extra, s->chars, s->length);
    } else {
        memcpy(out->chars, s->chars, s->length);
        memset(out->chars + s->length, fill, extra);
    }
    release(OBJ_VAL(s));
    return OBJ_VAL(out);
}

NATIVE(n_pad_left) { return pad(I, argc, argv, "pad_left", 1); }
NATIVE(n_pad_right) { return pad(I, argc, argv, "pad_right", 0); }

NATIVE(n_fixed) {
    NEED_NUM(0);
    if (argc > 1) NEED_INT(1);
    double digits = argc > 1 ? numeric_value(argv[1]) : 2;
    if (digits < 0 || digits > 20) return lube_error(I, "fixed(): digits must be between 0 and 20");
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", (int)digits, numeric_value(argv[0]));
    return OBJ_VAL(new_string(buf, strlen(buf)));
}

/* ======================================================================
 * Math
 * ==================================================================== */

#define MATH1(name, expr) \
    NATIVE(name) { (void)argc; NEED_NUM(0); double x = numeric_value(argv[0]); return NUM_VAL(expr); }

NATIVE(n_abs) {
    (void)argc;
    NEED_NUM(0);
    if (IS_INT(argv[0])) return int_abs(I, argv[0]);
    return NUM_VAL(fabs(argv[0].as.number));
}

/* floor, ceil and round leave a sized integer unchanged */
#define ROUNDING(name, expr) \
    NATIVE(name) { (void)argc; NEED_NUM(0); if (IS_INT(argv[0])) return argv[0]; \
                   double x = argv[0].as.number; return NUM_VAL(expr); }
ROUNDING(n_floor, floor(x))
ROUNDING(n_ceil, ceil(x))
MATH1(n_sqrt, sqrt(x))
MATH1(n_sin, sin(x))
MATH1(n_cos, cos(x))
MATH1(n_tan, tan(x))
MATH1(n_exp, exp(x))

NATIVE(n_atan2) {
    (void)argc;
    NEED_NUM(0);
    NEED_NUM(1);
    return NUM_VAL(atan2(numeric_value(argv[0]), numeric_value(argv[1])));
}

NATIVE(n_pow) {
    (void)argc;
    NEED_NUM(0);
    NEED_NUM(1);
    return NUM_VAL(pow(numeric_value(argv[0]), numeric_value(argv[1])));
}

NATIVE(n_log) {
    NEED_NUM(0);
    if (argc > 1) {
        NEED_NUM(1);
        return NUM_VAL(log(numeric_value(argv[0])) / log(numeric_value(argv[1])));
    }
    return NUM_VAL(log(numeric_value(argv[0])));
}

NATIVE(n_round) {
    NEED_NUM(0);
    if (IS_INT(argv[0])) return argv[0];
    double x = numeric_value(argv[0]);
    if (argc < 2) return NUM_VAL(lube_round(x));
    NEED_INT(1);
    double m = pow(10, numeric_value(argv[1]));
    return NUM_VAL(floor(x * m + 0.5) / m);
}

/* xorshift64* pseudo random generator */
static double next_random(Interp *I) {
    uint64_t x = I->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    I->rng = x;
    return (double)((x * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
}

NATIVE(n_random) { (void)argc; (void)argv; return NUM_VAL(next_random(I)); }

NATIVE(n_random_int) {
    (void)argc;
    NEED_INT(0);
    NEED_INT(1);
    double a = numeric_value(argv[0]), b = numeric_value(argv[1]);
    if (b < a) return lube_error(I, "random_int(): the first number must not be larger than the second");
    return NUM_VAL(a + floor(next_random(I) * (b - a + 1)));
}

NATIVE(n_seed) {
    (void)argc;
    NEED(IS_WHOLE(argv[0]) && fabs(numeric_value(argv[0])) < 9007199254740992.0, 0, "a whole number below 2^53");
    I->rng = (uint64_t)(int64_t)numeric_value(argv[0]) * 0x9E3779B97F4A7C15ULL + 1;
    if (I->rng == 0) I->rng = 1;
    return NIL_VAL;
}

NATIVE(n_choice) {
    (void)argc;
    LIST_ARG(l, 0);
    Value r = l->count == 0 ? lube_error(I, "choice() of an empty list")
                            : retain(l->items[(size_t)(next_random(I) * (double)l->count)]);
    release(OBJ_VAL(l));
    return r;
}

NATIVE(n_shuffle) {
    (void)argc;
    LIST_ARG(src, 0);
    ObjList *out = list_copy(src);
    release(OBJ_VAL(src));
    for (size_t i = out->count; i > 1; i--) {  /* Fisher-Yates */
        size_t j = (size_t)(next_random(I) * (double)i);
        Value t = out->items[i - 1];
        out->items[i - 1] = out->items[j];
        out->items[j] = t;
    }
    return OBJ_VAL(out);
}

/* ======================================================================
 * Sized integers: int32() is 0, int32(x) converts x (a value that does not
 * fit is an error; a fraction is dropped; strings like "42" are parsed).
 * ==================================================================== */

/* String(x), Number(x), Bool(x), Object(x), List(x), Function(f): conversions.
 * With no argument each gives its default value (Function has none). */
/* String(x): a String object holding x's text. String() holds "". */
NATIVE(n_String) {
    (void)I;
    if (argc == 0) return OBJ_VAL(new_strobj(new_string("", 0)));
    if (IS_STROBJ(argv[0])) return retain(argv[0]);
    return OBJ_VAL(new_strobj(value_to_string(argv[0])));
}

NATIVE(n_Number) {
    if (argc == 0) return NUM_VAL(0);
    return to_number(I, argc, argv, "Number");
}

NATIVE(n_Bool) {
    (void)I;
    return BOOL_VAL(argc > 0 && is_truthy(argv[0]));
}

/* Builds an Object (dict = 0) or a Map (dict = 1) from an Object, a Map or
 * a list of [key, value] pairs. */
static Value build_map(Interp *I, Value src, int dict, const char *fn) {
    ObjMap *m = new_map();
    m->dict = (unsigned char)dict;
    if (IS_MAP(src)) {
        ObjMap *from = AS_MAP(src);
        for (size_t i = 0; i < from->used; i++) {
            if (!from->entries[i].live) continue;
            if (!map_key_ok(m, from->entries[i].key)) {
                release(OBJ_VAL(m));
                return lube_error(I, "%s(): %s cannot be an object key (keys must be str, Number or Bool)", fn,
                                  a_type(from->entries[i].key));
            }
            map_set(m, retain(from->entries[i].key), retain(from->entries[i].value));
        }
        return OBJ_VAL(m);
    }
    if (!IS_LIST(src)) {
        release(OBJ_VAL(m));
        return type_error(I, fn, 0, "an Object, a Map or a List of [key, value] pairs", src);
    }
    ObjList *pairs = AS_LIST(src);
    for (size_t i = 0; i < pairs->count; i++) {
        Value p = pairs->items[i];
        if (!IS_LIST(p) || AS_LIST(p)->count != 2 || !map_key_ok(m, AS_LIST(p)->items[0])) {
            release(OBJ_VAL(m));
            return lube_error(I, dict ? "%s(): item %lu is not a [key, value] pair"
                                      : "%s(): item %lu is not a [key, value] pair with a str, Number or Bool key",
                              fn, (unsigned long)i);
        }
        map_set(m, retain(AS_LIST(p)->items[0]), retain(AS_LIST(p)->items[1]));
    }
    return OBJ_VAL(m);
}

/* Object(x): an Object from an Object, a Map or [key, value] pairs. */
NATIVE(n_Object) {
    if (argc == 0) return OBJ_VAL(new_map());
    if (IS_MAP(argv[0]) && !AS_MAP(argv[0])->dict) return retain(argv[0]);
    return build_map(I, argv[0], 0, "Object");
}

/* Map(x): a Map (keys of any type) from an Object, a Map or pairs. */
NATIVE(n_Map) {
    if (argc == 0) {
        ObjMap *m = new_map();
        m->dict = 1;
        return OBJ_VAL(m);
    }
    return build_map(I, argv[0], 1, "Map");
}

/* List(x) makes a plain list (any items) from a list, range, string or object. */
NATIVE(n_List) {
    if (argc == 0) return OBJ_VAL(new_list());
    ObjList *l = to_list(I, argv[0], "List");
    if (!l) return ERR_VAL;
    if (l->elem == TT_ANY) return OBJ_VAL(l);
    ObjList *plain = list_copy(l);
    plain->elem = TT_ANY;
    release(OBJ_VAL(l));
    return OBJ_VAL(plain);
}

NATIVE(n_Function) {
    (void)argc;
    NEED_FN(0);
    return retain(argv[0]);
}

#define INT_TYPE(name, kind) \
    NATIVE(name) { return argc == 0 ? int_convert(I, NUM_VAL(0), kind) : int_convert(I, argv[0], kind); }
INT_TYPE(n_int8, IK_I8)
INT_TYPE(n_int16, IK_I16)
INT_TYPE(n_int32, IK_I32)
INT_TYPE(n_int64, IK_I64)
INT_TYPE(n_uint8, IK_U8)
INT_TYPE(n_uint16, IK_U16)
INT_TYPE(n_uint32, IK_U32)
INT_TYPE(n_uint64, IK_U64)

/* ======================================================================
 * Misc
 * ==================================================================== */

/* hot_reload(): applies the files of the program that changed since it
 * started (or since the last reload) and returns their paths. lube --hot
 * does this by itself while the program runs. */
NATIVE(n_hot_reload) {
    (void)argc;
    (void)argv;
    return OBJ_VAL(lube_hot_reload(I));
}

NATIVE(n_assert) {
    if (is_truthy(argv[0])) return NIL_VAL;
    if (argc > 1) {
        ObjString *msg = value_to_string(argv[1]);
        Value e = lube_error(I, "assertion failed: %s", msg->chars);
        release(OBJ_VAL(msg));
        return e;
    }
    return lube_error(I, "assertion failed");
}

static const struct {
    const char *name;
    NativeFn fn;
    int min, max;
    int changes_first;
} BUILTINS[] = {
    /* input / output */
    LUBE_BUILTIN(print, n_print, 0, -1, 0)
    LUBE_BUILTIN(write, n_write, 0, -1, 0)
    LUBE_BUILTIN(input, n_input, 0, 1, 0)
    LUBE_BUILTIN(read_file, n_read_file, 1, 1, 0)
    LUBE_BUILTIN(write_file, n_write_file, 2, 2, 0)
    LUBE_BUILTIN(append_file, n_append_file, 2, 2, 0)
    LUBE_BUILTIN(exit, n_exit, 0, 1, 0)
    LUBE_BUILTIN(clock, n_clock, 0, 0, 0)
    LUBE_BUILTIN(time, n_time, 0, 0, 0)
    /* types */
    LUBE_BUILTIN(type, n_type, 1, 1, 0)
    LUBE_BUILTIN(str, n_str, 1, 1, 0)
    LUBE_BUILTIN(repr, n_repr, 1, 1, 0)
    LUBE_BUILTIN(num, n_num, 1, 2, 0)
    LUBE_BUILTIN(int, n_int, 1, 2, 0)
    LUBE_BUILTIN(bool, n_bool, 1, 1, 0)
    LUBE_BUILTIN(String, n_String, 0, 1, 0)
    LUBE_BUILTIN(Number, n_Number, 0, 2, 0)
    LUBE_BUILTIN(Bool, n_Bool, 0, 1, 0)
    LUBE_BUILTIN(Object, n_Object, 0, 1, 0)
    LUBE_BUILTIN(Map, n_Map, 0, 1, 0)
    LUBE_BUILTIN(List, n_List, 0, 1, 0)
    LUBE_BUILTIN(Function, n_Function, 1, 1, 0)
    LUBE_BUILTIN(Range, n_range, 1, 3, 0)
    LUBE_BUILTIN(int8, n_int8, 0, 1, 0)
    LUBE_BUILTIN(int16, n_int16, 0, 1, 0)
    LUBE_BUILTIN(int32, n_int32, 0, 1, 0)
    LUBE_BUILTIN(int64, n_int64, 0, 1, 0)
    LUBE_BUILTIN(uint8, n_uint8, 0, 1, 0)
    LUBE_BUILTIN(uint16, n_uint16, 0, 1, 0)
    LUBE_BUILTIN(uint32, n_uint32, 0, 1, 0)
    LUBE_BUILTIN(uint64, n_uint64, 0, 1, 0)
    /* changing collections in place (first argument taken with &) */
    LUBE_BUILTIN(push, n_push, 2, -1, 1)
    LUBE_BUILTIN(pop, n_pop, 1, 1, 1)
    LUBE_BUILTIN(insert, n_insert, 3, 3, 1)
    LUBE_BUILTIN(remove, n_remove, 2, 2, 1)
    LUBE_BUILTIN(delete, n_delete, 2, 2, 1)
    LUBE_BUILTIN(clear, n_clear, 1, 1, 1)
    /* collections */
    LUBE_BUILTIN(len, n_len, 1, 1, 0)
    LUBE_BUILTIN(slice, n_slice, 1, 3, 0)
    LUBE_BUILTIN(reverse, n_reverse, 1, 1, 0)
    LUBE_BUILTIN(sort, n_sort, 1, 2, 0)
    LUBE_BUILTIN(contains, n_contains, 2, 2, 0)
    LUBE_BUILTIN(index_of, n_index_of, 2, 2, 0)
    LUBE_BUILTIN(join, n_join, 1, 2, 0)
    LUBE_BUILTIN(keys, n_keys, 1, 1, 0)
    LUBE_BUILTIN(values, n_values, 1, 1, 0)
    LUBE_BUILTIN(items, n_items, 1, 1, 0)
    LUBE_BUILTIN(has, n_has, 2, 2, 0)
    LUBE_BUILTIN(get, n_get, 2, 3, 0)
    LUBE_BUILTIN(list, n_list, 0, 1, 0)
    LUBE_BUILTIN(range, n_range, 1, 3, 0)
    LUBE_BUILTIN(map, n_map, 2, 2, 0)
    LUBE_BUILTIN(filter, n_filter, 2, 2, 0)
    LUBE_BUILTIN(each, n_each, 2, 2, 0)
    LUBE_BUILTIN(reduce, n_reduce, 2, 3, 0)
    LUBE_BUILTIN(find, n_find, 2, 3, 0)
    LUBE_BUILTIN(find_index, n_find_index, 2, 2, 0)
    LUBE_BUILTIN(any, n_any, 1, 2, 0)
    LUBE_BUILTIN(all, n_all, 1, 2, 0)
    LUBE_BUILTIN(sum, n_sum, 1, 1, 0)
    LUBE_BUILTIN(min, n_min, 1, -1, 0)
    LUBE_BUILTIN(max, n_max, 1, -1, 0)
    LUBE_BUILTIN(zip, n_zip, 2, 2, 0)
    /* strings */
    LUBE_BUILTIN(upper, n_upper, 1, 1, 0)
    LUBE_BUILTIN(lower, n_lower, 1, 1, 0)
    LUBE_BUILTIN(trim, n_trim, 1, 1, 0)
    LUBE_BUILTIN(split, n_split, 1, 2, 0)
    LUBE_BUILTIN(replace, n_replace, 3, 3, 0)
    LUBE_BUILTIN(starts_with, n_starts_with, 2, 2, 0)
    LUBE_BUILTIN(ends_with, n_ends_with, 2, 2, 0)
    LUBE_BUILTIN(chars, n_chars, 1, 1, 0)
    LUBE_BUILTIN(ord, n_ord, 1, 1, 0)
    LUBE_BUILTIN(chr, n_chr, 1, 1, 0)
    LUBE_BUILTIN(pad_left, n_pad_left, 2, 3, 0)
    LUBE_BUILTIN(pad_right, n_pad_right, 2, 3, 0)
    LUBE_BUILTIN(fixed, n_fixed, 1, 2, 0)
    /* math */
    LUBE_BUILTIN(abs, n_abs, 1, 1, 0)
    LUBE_BUILTIN(floor, n_floor, 1, 1, 0)
    LUBE_BUILTIN(ceil, n_ceil, 1, 1, 0)
    LUBE_BUILTIN(round, n_round, 1, 2, 0)
    LUBE_BUILTIN(sqrt, n_sqrt, 1, 1, 0)
    LUBE_BUILTIN(pow, n_pow, 2, 2, 0)
    LUBE_BUILTIN(sin, n_sin, 1, 1, 0)
    LUBE_BUILTIN(cos, n_cos, 1, 1, 0)
    LUBE_BUILTIN(tan, n_tan, 1, 1, 0)
    LUBE_BUILTIN(atan2, n_atan2, 2, 2, 0)
    LUBE_BUILTIN(log, n_log, 1, 2, 0)
    LUBE_BUILTIN(exp, n_exp, 1, 1, 0)
    LUBE_BUILTIN(random, n_random, 0, 0, 0)
    LUBE_BUILTIN(random_int, n_random_int, 2, 2, 0)
    LUBE_BUILTIN(seed, n_seed, 1, 1, 0)
    LUBE_BUILTIN(choice, n_choice, 1, 1, 0)
    LUBE_BUILTIN(shuffle, n_shuffle, 1, 1, 0)
    /* misc */
    LUBE_BUILTIN(assert, n_assert, 1, 2, 0)
    LUBE_BUILTIN(hot_reload, n_hot_reload, 0, 0, 0)
    {NULL, NULL, 0, 0, 0}  /* (the end) */
};

void register_builtins(Interp *I) {
    for (size_t i = 0; BUILTINS[i].name; i++) {
        ObjNative *n = new_native(BUILTINS[i].name, BUILTINS[i].fn, BUILTINS[i].min, BUILTINS[i].max,
                                  BUILTINS[i].changes_first);
        lube_define(I, BUILTINS[i].name, OBJ_VAL(n));
    }
    lube_define(I, "PI", NUM_VAL(3.14159265358979323846));
    lube_define(I, "INF", NUM_VAL(HUGE_VAL));
}
