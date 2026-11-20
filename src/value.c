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

uint32_t string_hash(ObjString *s) {
    if (!s->hashed) {
        s->hash = hash_bytes(s->chars, s->length);
        s->hashed = 1;
    }
    return s->hash;
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

/* Returns the shared copy of a string. The table owns it; the caller gets a
 * borrowed pointer that stays valid until the interpreter shuts down. */
ObjString *lube_intern(Interp *I, const char *chars, size_t length) {
    uint32_t h = hash_bytes(chars, length);
    ObjMap *m = I->strings;
    if (m->index_cap) {
        size_t mask = m->index_cap - 1, i = h & mask;
        for (;;) {
            int32_t e = m->index[i];
            if (e == -1) break;
            if (e >= 0 && m->entries[e].hash == h) {
                ObjString *s = AS_STRING(m->entries[e].key);
                if (s->length == length && memcmp(s->chars, chars, length) == 0) return s;
            }
            i = (i + 1) & mask;
        }
    }
    ObjString *s = new_string(chars, length);
    map_set(m, OBJ_VAL(s), BOOL_VAL(1));
    return s;
}

long str_find(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t from) {
    if (nlen == 0) return from <= hlen ? (long)from : -1;
    if (nlen > hlen || from > hlen - nlen) return -1;
    const char *p = hay + from, *end = hay + (hlen - nlen) + 1;  /* (the first byte by memchr: quick) */
    while (p < end && (p = memchr(p, needle[0], (size_t)(end - p))) != NULL) {
        if (memcmp(p, needle, nlen) == 0) return (long)(p - hay);
        p++;
    }
    return -1;
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

void list_push(ObjList *l, Value v) {
    list_reserve(l, l->count + 1);
    l->items[l->count++] = v;
}

ObjList *list_copy(ObjList *src) {
    ObjList *l = new_list();
    list_reserve(l, src->count);
    for (size_t i = 0; i < src->count; i++) l->items[i] = retain(src->items[i]);
    l->count = src->count;
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
    if (IS_TEXT(v)) return string_hash(text_of(v));  /* "a" == String("a"), so they hash alike */
    if (IS_BOOL(v)) return v.as.boolean ? 0x2545f491u : 0x6c8e9cf5u;
    if (IS_NUM(v)) return hash_double(v.as.number);
    if (IS_INT(v)) return int_exact_double(v, &d) ? hash_double(d) : mix64(v.as.u ^ 0x5bd1e995u);
    uint32_t h = (uint32_t)OBJ_TYPE(v) * 0x9e3779b9u;
    if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        h ^= (uint32_t)l->count;
        for (size_t i = 0; i < l->count && depth < HASH_DEPTH; i++) h = h * 31u + hash_at(l->items[i], depth + 1);
    } else if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        h ^= (uint32_t)m->count ^ (uint32_t)((uintptr_t)m->klass >> 4);
        for (size_t i = 0; i < m->used && depth < HASH_DEPTH; i++)
            if (m->entries[i].live)  /* a sum does not depend on the order */
                h += mix64(((uint64_t)hash_at(m->entries[i].key, depth + 1) << 32) | hash_at(m->entries[i].value, depth + 1));
    } else if (IS_RANGE(v)) {
        ObjRange *r = AS_RANGE(v);
        h ^= hash_double(r->start) + 3u * hash_double(r->end) + 7u * hash_double(r->step) + (uint32_t)r->inclusive;
    } else {
        h ^= mix64((uint64_t)(uintptr_t)v.as.obj);  /* functions: by identity */
    }
    return h;
}

static uint32_t hash_value(Value v) { return hash_at(v, 0); }

/* Called only when the hashes match. */
static int keys_equal(Value a, Value b) {
    if (IS_TEXT(a) && IS_TEXT(b)) {
        ObjString *x = text_of(a), *y = text_of(b);
        return x == y || (x->length == y->length && memcmp(x->chars, y->chars, x->length) == 0);
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
    m->klass = NULL;
    return m;
}

/* Returns the index-table slot holding `key`, or -1. */
static long map_probe(ObjMap *m, Value key, uint32_t h) {
    if (m->index_cap == 0) return -1;
    size_t mask = m->index_cap - 1, i = h & mask;
    for (;;) {
        int32_t e = m->index[i];
        if (e == SLOT_EMPTY) return -1;
        if (e >= 0 && m->entries[e].hash == h && keys_equal(m->entries[e].key, key)) return (long)i;
        i = (i + 1) & mask;
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

    /* index entries are 32-bit, and sizes must not overflow */
    if (want > ((size_t)1 << 30) || want > LUBE_MAX_ITEMS) lube_out_of_memory();
    size_t cap = 8;
    while (cap < want) cap *= 2;
    m->entries = xrealloc(m->entries, cap * sizeof(MapEntry));
    m->entry_cap = cap;
    size_t icap = cap * 2;
    m->index = xrealloc(m->index, icap * sizeof(int32_t));
    m->index_cap = icap;
    for (size_t i = 0; i < icap; i++) m->index[i] = SLOT_EMPTY;
    size_t mask = icap - 1;
    for (size_t e = 0; e < m->used; e++) {
        size_t i = m->entries[e].hash & mask;
        while (m->index[i] != SLOT_EMPTY) i = (i + 1) & mask;
        m->index[i] = (int32_t)e;
    }
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

void map_set(ObjMap *m, Value key, Value value) {
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
    while (m->index[i] >= 0) i = (i + 1) & mask;  /* first empty or deleted slot */
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
    /* only lists and objects are ever changed in place */
    if (!(IS_LIST(*slot) || IS_MAP(*slot)) || slot->as.obj->rc <= 1) return;
    Value copy = IS_LIST(*slot) ? OBJ_VAL(list_copy(AS_LIST(*slot))) : OBJ_VAL(map_copy(AS_MAP(*slot)));
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
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
};
static const char *LIST_NAMES[TT_COUNT] = {
    "List", "str[]", "String[]", "Number[]", "Bool[]", "Object[]", "Map[]", "List[]", "Function[]", "Range[]",
    "Channel[]", "Isolate[]", "int8[]", "int16[]", "int32[]", "int64[]", "uint8[]", "uint16[]", "uint32[]", "uint64[]",
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
    else if (IS_INT(v)) { d.kind = TD_INT; d.a = v.ikind; }
    else if (IS_OBJ(v)) {
        switch (OBJ_TYPE(v)) {
        case OBJ_STRING: d.kind = TD_STR; break;
        case OBJ_STROBJ: d.kind = TD_STROBJ; break;
        case OBJ_LIST: d.kind = TD_LIST; d.a = (unsigned char)AS_LIST(v)->elem; break;
        case OBJ_RANGE: d.kind = TD_RANGE; break;
        case OBJ_CLASS: d.kind = TD_CLASSVAL; break;
        case OBJ_CHANNEL: d.kind = TD_CHANNEL; break;
        case OBJ_ISOLATE: d.kind = TD_ISOLATE; break;
        case OBJ_PACKAGE: d.kind = TD_OBJECT; break;
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
    case TD_ISOLATE: return IS_ISOLATE(v);
    case TD_CLASSVAL: return IS_CLASS(v);
    default: return IS_INSTANCE(v) && is_subclass(AS_MAP(v)->klass, d.cls);  /* TD_CLASS */
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
            ObjString *p = text_of(x), *q = text_of(y);
            equal = p->length == q->length && memcmp(p->chars, q->chars, p->length) == 0;
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
    if (!IS_OBJ(v)) return "nothing"; /* LCOV_EXCL_LINE: "no value" never reaches a type check */
    switch (OBJ_TYPE(v)) {
    case OBJ_STRING: return "str";
    case OBJ_STROBJ: return "String";
    case OBJ_LIST: return LIST_NAMES[AS_LIST(v)->elem];
    case OBJ_MAP:
        if (AS_MAP(v)->klass) return AS_MAP(v)->klass->name->chars;
        return AS_MAP(v)->dict ? map_name(AS_MAP(v)->ktype, AS_MAP(v)->vtype) : "Object";
    case OBJ_RANGE: return "Range";
    case OBJ_CLASS: return "Class";
    case OBJ_CHANNEL: return "Channel";
    case OBJ_ISOLATE: return "Isolate";
    case OBJ_PACKAGE: return "Package";
    default: return "Function";  /* OBJ_FUNCTION or OBJ_NATIVE */
    }
}

/* ======================================================================
 * Printing
 * ==================================================================== */

/* Integers print without a decimal point; other numbers use the shortest
 * representation that reads back as the same double. */
void format_number(char *buf, size_t size, double n) {
    if (isnan(n)) {
        snprintf(buf, size, "nan");
    } else if (isinf(n)) {
        snprintf(buf, size, n > 0 ? "inf" : "-inf");
    } else if (n == floor(n) && fabs(n) < 1e16) {  /* a whole number: its digits (exact in an int64; -0 is 0) */
        char digits[24];
        int len = 0;
        long long k = (long long)n;
        unsigned long long u = k < 0 ? 0ull - (unsigned long long)k : (unsigned long long)k;
        do {
            digits[len++] = (char)('0' + u % 10);
            u /= 10;
        } while (u);
        if (k < 0) digits[len++] = '-';
        size_t i = 0;
        while (len > 0 && i + 1 < size) buf[i++] = digits[--len];
        buf[i] = '\0';
    } else {
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
    switch (v.type) {
    case VAL_BOOL: sb_puts(sb, v.as.boolean ? "true" : "false"); return;
    case VAL_NUM:
        format_number(num, sizeof num, v.as.number);
        sb_puts(sb, num);
        return;
    case VAL_INT:
        int_format(num, sizeof num, v);
        sb_puts(sb, num);
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
            if (IS_STRING(e->key) && is_bare_key(AS_STRING(e->key)))
                sb_putn(sb, AS_STRING(e->key)->chars, AS_STRING(e->key)->length);
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
        sb_printf(sb, "<builtin %s>", AS_NATIVE(v)->name);
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
    if (IS_TEXT(v)) return (ObjString *)retain(OBJ_VAL(text_of(v))).as.obj;
    StrBuf sb;
    sb_init(&sb);
    append_value(&sb, v, 0);
    ObjString *s = new_string(sb.data, sb.len);
    sb_free(&sb);
    return s;
}

/* The quoted form of v, as the REPL shows it (a new reference). */
ObjString *value_repr(Value v) {
    StrBuf sb;
    sb_init(&sb);
    append_value(&sb, v, 1);
    ObjString *s = new_string(sb.data, sb.len);
    sb_free(&sb);
    return s;
}
