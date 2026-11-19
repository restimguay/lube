/*
 * interp.c - the tree-walking interpreter.
 *
 * Ownership rules (there is no garbage collector):
 *   - eval() returns a new reference, which the caller must release, or
 *     ERR_VAL when an error was raised (the error is in I->error_val).
 *   - Errors are ordinary return values that every caller checks, so each
 *     function releases what it holds on every path. Nothing jumps over
 *     cleanup code.
 *   - Parameters and locals live in I->stack, one frame per call. Leaving a
 *     block clears its slots, so values are freed as they go out of scope.
 *   - A list or map is changed in place only when it has a single owner
 *     (make_unique). Everyone else keeps seeing their own value.
 *
 * Changing a caller's variable: `f(&x)` lends x to f. The value is moved out
 * of x (no copy), f changes it, and the result is moved back when f returns,
 * whether it returns normally or with an error. While lent, a global holds a
 * VAL_LENT marker so any other use of it is reported instead of seen half-done.
 */
#include "lube.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LUBE_STACK_SIZE
#define LUBE_STACK_SIZE (8u * 1024u * 1024u)
#endif

typedef enum { EX_NORMAL, EX_BREAK, EX_CONTINUE, EX_RETURN, EX_ERROR } Exec;

static Value eval(Interp *I, Node *n);
static Value eval_value(Interp *I, Node *n);
static Value bad_key(Interp *I, Node *at, ObjMap *m, Value key);
static Exec exec(Interp *I, Node *n);
static Exec exec_stmts(Interp *I, NodeList *stmts);

#define SLOT(i) (I->stack[I->frame->base + (i)])
#define MAX_TRACE 12
#define IN_USE "'%s' is in use: it was passed with & to a call that has not returned yet"

/* ======================================================================
 * Errors
 * ==================================================================== */

static void trace_reset(Interp *I) {
    I->trace.len = 0;
    if (I->trace.data) I->trace.data[0] = '\0';
    I->trace_count = I->trace_shown = I->trace_hidden = I->trace_repeat = 0;
    I->trace_prev[0] = '\0';
    I->trace_last[0] = '\0';
}

/* Raises v (taking over the reference). Always returns ERR_VAL. */
Value lube_throw(Interp *I, Value v) {
    release(I->error_val);
    I->error_val = v;
    I->error_line = I->frame ? I->frame->line : 0;
    I->error_src = I->frame ? I->frame->src : NULL;
    trace_reset(I);
    return ERR_VAL;
}

LUBE_COLD static Value verror(Interp *I, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return lube_throw(I, OBJ_VAL(new_string(buf, strlen(buf))));
}

Value lube_error(Interp *I, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Value r = verror(I, fmt, ap);
    va_end(ap);
    return r;
}

/* Raises an error located at node n. */
LUBE_COLD static Value error_at(Interp *I, Node *n, const char *fmt, ...) {
    if (I->frame && n) I->frame->line = n->line;
    va_list ap;
    va_start(ap, fmt);
    Value r = verror(I, fmt, ap);
    va_end(ap);
    return r;
}

LUBE_COLD static Value cannot_change(Interp *I, Node *at, const char *name, Origin origin) {
    char msg[400];
    unchangeable_message(msg, sizeof msg, name, origin);
    return error_at(I, at, "%s", msg);
}

/* Notes how often the previous traceback line repeated (deep recursion
 * would otherwise print the same line thousands of times). */
static void trace_flush_repeats(Interp *I) {
    int n = I->trace_repeat;
    if (n == 0) return;
    I->trace_repeat = 0;
    if (I->trace_hidden == 0) sb_printf(&I->trace, "  ... repeated %d more time%s\n", n, n == 1 ? "" : "s");
    else I->trace_hidden += n;
}

/* Called for each frame an error passes through on its way out. Keeps the
 * first MAX_TRACE distinct lines and the outermost frame. */
LUBE_COLD static void trace_frame(Interp *I, Frame *f) {
    char line[sizeof I->trace_prev];
    snprintf(line, sizeof line, "  at %s (%s:%d)\n", f->name, f->src ? f->src->path : "?", f->line);
    I->trace_count++;
    if (strcmp(line, I->trace_prev) == 0) {
        I->trace_repeat++;
        return;
    }
    trace_flush_repeats(I);
    memcpy(I->trace_prev, line, sizeof line);
    if (I->trace_shown < MAX_TRACE && I->trace_hidden == 0) {
        sb_puts(&I->trace, line);
        I->trace_shown++;
    } else {
        I->trace_hidden++;
        memcpy(I->trace_last, line, sizeof line);
    }
}

/* ======================================================================
 * Stack
 * ==================================================================== */

static int stack_room(Interp *I, int n) { return I->sp + n <= LUBE_STACK_MAX; }

/* Releases stack slots down to `sp`. */
static void pop_to(Interp *I, int sp) {
    while (I->sp > sp) {
        I->sp--;
        Value v = I->stack[I->sp];
        I->stack[I->sp] = NIL_VAL;
        release(v);
    }
}

/* Clears slots [first, first+count) of the current frame. */
static void clear_slots(Interp *I, int first, int count) {
    Value *s = &I->stack[I->frame->base + first];
    for (int i = 0; i < count; i++) {
        Value v = s[i];
        s[i] = NIL_VAL;
        release(v);
    }
}

static void set_slot(Interp *I, int slot, Value v) {
    Value old = SLOT(slot);
    SLOT(slot) = v;
    release(old);
}

/* Is there still room on the C stack? Every recursive part of the
 * interpreter checks this, so deep input gives an error, never a crash. */
int lube_stack_ok(Interp *I) {
    char here;
    return (size_t)(I->stack_base - &here) <= I->stack_budget;
}

/* ======================================================================
 * Variables
 * ==================================================================== */

static int global_origin(Interp *I, ObjString *name, Origin *origin) {
    Value v;
    if (!map_get(I->constants, OBJ_VAL(name), &v)) return 0;
    *origin = (Origin)(int)v.as.number;
    return 1;
}

/* The declared type of a global made with let or var, or NULL. The pointer
 * is only valid until the next global is declared. */
static TypeDesc *global_type(Interp *I, ObjString *name) {
    Value idx;
    if (!map_get(I->global_type_index, OBJ_VAL(name), &idx)) return NULL;
    return &I->global_types[(int)idx.as.number];
}

static void set_global_type(Interp *I, ObjString *name, TypeDesc d) {
    TypeDesc *t = global_type(I, name);
    if (t) {
        *t = d;
        return;
    }
    if (I->nglobal_types == I->cap_global_types) {
        I->cap_global_types = I->cap_global_types ? I->cap_global_types * 2 : 16;
        I->global_types = xrealloc(I->global_types, sizeof(TypeDesc) * (size_t)I->cap_global_types);
    }
    I->global_types[I->nglobal_types] = d;
    map_set(I->global_type_index, retain(OBJ_VAL(name)), NUM_VAL(I->nglobal_types));
    I->nglobal_types++;
}

/* A var keeps the type it starts with (values of a subtype are fine).
 * Takes over v; returns the value to store, or raises an error. */
static Value keep_type(Interp *I, Node *at, VarRef *r, Value v) {
    TypeDesc *d = r->kind == VAR_LOCAL ? &I->decl[I->frame->base + r->index] : global_type(I, r->name);
    Value out;
    if (!d || desc_accepts(*d, v, &out)) return d ? out : v;
    const char *hint = d->kind == TD_STROBJ && IS_STRING(v) ? " (convert it with String(x))"
                       : d->kind == TD_STR && IS_STROBJ(v) ? " (use x.value for its str)" : "";
    Value e = error_at(I, at, "'%s' is %s, so it cannot hold %s: a var keeps the type it starts with%s",
                       r->name->chars, with_article(desc_name(*d)), a_type(v), hint);
    release(v);
    return e;
}

/* A class field keeps the type it starts with. Takes over v. */
static Value keep_field_type(Interp *I, Node *at, const TypeDesc *d, Value field, const char *holder, Value v) {
    Value out;
    if (desc_accepts(*d, v, &out)) return out;
    Value e = error_at(I, at, "field '%s' of %s is %s, so it cannot hold %s", AS_STRING(field)->chars, holder,
                       with_article(desc_name(*d)), a_type(v));
    release(v);
    return e;
}

static int keys_same(Value a, Value b) {
    ObjString *x = AS_STRING(a), *y = AS_STRING(b);
    return x == y || (x->length == y->length && memcmp(x->chars, y->chars, x->length) == 0);
}

static Value find_method(ObjClass *c, ObjString *name) {
    Value m;
    for (; c; c = c->parent)
        if (map_get(c->methods, OBJ_VAL(name), &m)) return m;
    return NIL_VAL;
}

static Value get_var(Interp *I, Node *n, VarRef *r) {
    switch (r->kind) {
    case VAR_LOCAL: return retain(SLOT(r->index));
    case VAR_CAPTURE: return retain(I->frame->fn->captures[r->index]);
    case VAR_SELF: return retain(OBJ_VAL(I->frame->fn));
    case VAR_GLOBAL: break;
    }
    Value *g = map_find(I->globals, OBJ_VAL(r->name));
    if (!g) return error_at(I, n, "undefined variable '%s'", r->name->chars);
    if (g->type == VAL_LENT) return error_at(I, n, IN_USE, r->name->chars);
    return retain(*g);
}

/* ======================================================================
 * Operators
 * ==================================================================== */

/* The operators that can fail with "unsupported operand types". */
static const char *op_symbol(TokenType op) {
    switch (op) {
    case T_PLUS: return "+";
    case T_MINUS: return "-";
    case T_STAR: return "*";
    case T_SLASH: return "/";
    case T_PERCENT: return "%";
    case T_POW: return "**";
    case T_LT: return "<";
    case T_LE: return "<=";
    case T_GT: return ">";
    default: return ">=";  /* T_GE */
    }
}

static TokenType compound_base(TokenType op) {
    switch (op) {
    case T_PLUS_EQ: return T_PLUS;
    case T_MINUS_EQ: return T_MINUS;
    case T_STAR_EQ: return T_STAR;
    case T_SLASH_EQ: return T_SLASH;
    default: return T_PERCENT;  /* T_PERCENT_EQ */
    }
}

/* Joins two values as text. With a String on the left the result is a
 * String; otherwise it is a str. */
static Value concat(Value a, Value b) {
    ObjString *x = value_to_string(a), *y = value_to_string(b);
    ObjString *s = alloc_string(x->length + y->length);
    memcpy(s->chars, x->chars, x->length);
    memcpy(s->chars + x->length, y->chars, y->length);
    release(OBJ_VAL(x));
    release(OBJ_VAL(y));
    return IS_STROBJ(a) ? OBJ_VAL(new_strobj(s)) : OBJ_VAL(s);
}

static Value repeat(Interp *I, Node *n, Value seq, double times) {
    if (!(times >= 0) || times != floor(times)) return error_at(I, n, "can only repeat by a whole, non-negative number");
    size_t unit = IS_TEXT(seq) ? text_of(seq)->length : AS_LIST(seq)->count;
    size_t limit = IS_TEXT(seq) ? LUBE_MAX_BYTES : LUBE_MAX_ITEMS;
    if (unit == 0) times = 0;
    if (times > (double)(limit / (unit ? unit : 1))) return error_at(I, n, "the repeated result would be too large");
    size_t k = (size_t)times;  /* safe: checked against the limit above */
    if (IS_TEXT(seq)) {
        ObjString *s = text_of(seq);
        ObjString *r = alloc_string(s->length * k);
        for (size_t i = 0; i < k; i++) memcpy(r->chars + i * s->length, s->chars, s->length);
        return IS_STROBJ(seq) ? OBJ_VAL(new_strobj(r)) : OBJ_VAL(r);
    }
    ObjList *src = AS_LIST(seq);
    ObjList *out = new_list();
    out->elem = src->elem;
    list_reserve(out, src->count * k);
    for (size_t i = 0; i < k; i++)
        for (size_t j = 0; j < src->count; j++) out->items[out->count++] = retain(src->items[j]);
    return OBJ_VAL(out);
}

static int compare_strings(ObjString *a, ObjString *b) {
    size_t n = a->length < b->length ? a->length : b->length;
    int c = memcmp(a->chars, b->chars, n);
    if (c != 0) return c;
    return a->length < b->length ? -1 : a->length > b->length;
}

static Value in_op(Interp *I, Value item, Value container) {
    int r = lube_contains(I, container, item);
    return r < 0 ? ERR_VAL : BOOL_VAL(r);
}

/* Borrows a and b; returns a new reference. */
static Value binary_op(Interp *I, Node *n, TokenType op, Value a, Value b) {
    if (IS_NUM(a) && IS_NUM(b)) {
        double x = a.as.number, y = b.as.number;
        switch (op) {
        case T_PLUS: return NUM_VAL(x + y);
        case T_MINUS: return NUM_VAL(x - y);
        case T_STAR: return NUM_VAL(x * y);
        case T_SLASH:
            if (y == 0) return error_at(I, n, "division by zero");
            return NUM_VAL(x / y);
        case T_PERCENT: {
            if (y == 0) return error_at(I, n, "modulo by zero");
            double r = fmod(x, y);
            if (r != 0 && ((r < 0) != (y < 0))) r += y;  /* result takes the sign of y */
            return NUM_VAL(r);
        }
        case T_POW: return NUM_VAL(pow(x, y));
        case T_LT: return BOOL_VAL(x < y);
        case T_LE: return BOOL_VAL(x <= y);
        case T_GT: return BOOL_VAL(x > y);
        case T_GE: return BOOL_VAL(x >= y);
        case T_EQ: return BOOL_VAL(x == y);
        case T_NEQ: return BOOL_VAL(x != y);
        default: break;
        }
    }
    if ((IS_INT(a) || IS_INT(b)) && IS_NUMERIC(a) && IS_NUMERIC(b)) {
        if (I->frame && n) I->frame->line = n->line;
        int c;
        switch (op) {
        case T_LT: case T_LE: case T_GT: case T_GE:
            c = numeric_compare(a, b);
            if (c == 2) return BOOL_VAL(0);
            return BOOL_VAL(op == T_LT ? c < 0 : op == T_LE ? c <= 0 : op == T_GT ? c > 0 : c >= 0);
        case T_EQ: return BOOL_VAL(numeric_compare(a, b) == 0);
        case T_NEQ: return BOOL_VAL(numeric_compare(a, b) != 0);
        case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: case T_POW:
            return int_arith(I, op, a, b);
        default: break;
        }
    }
    switch (op) {
    case T_EQ: return BOOL_VAL(values_equal(a, b));
    case T_NEQ: return BOOL_VAL(!values_equal(a, b));
    case T_IN:
        if (I->frame && n) I->frame->line = n->line;
        return in_op(I, a, b);
    case T_PLUS:
        if (IS_TEXT(a) || IS_TEXT(b)) return concat(a, b);
        if (IS_LIST(a) && IS_LIST(b)) {  /* keeps the left list's element type */
            ObjList *x = AS_LIST(a), *y = AS_LIST(b);
            ObjList *out = new_list();
            out->elem = x->elem;
            list_reserve(out, x->count + y->count);
            for (size_t i = 0; i < x->count; i++) out->items[out->count++] = retain(x->items[i]);
            for (size_t i = 0; i < y->count; i++) {
                Value v = lube_check_elem(I, type_name(a), out->elem, y->items[i]);
                if (IS_ERR(v)) {
                    release(OBJ_VAL(out));
                    return v;
                }
                out->items[out->count++] = v;
            }
            return OBJ_VAL(out);
        }
        if (IS_MAP(a) && IS_MAP(b)) {  /* merge: keys from b win; the result has a's kind and types */
            ObjMap *out = map_copy(AS_MAP(a));
            ObjMap *y = AS_MAP(b);
            for (size_t i = 0; i < y->used; i++) {
                if (!y->entries[i].live) continue;
                Value k = y->entries[i].key;
                Value v = map_key_ok(out, k) ? lube_check_elem(I, type_name(a), out->vtype, y->entries[i].value)
                                             : bad_key(I, n, out, k);
                if (IS_ERR(v)) {
                    release(OBJ_VAL(out));
                    return v;
                }
                map_set(out, retain(k), v);
            }
            return OBJ_VAL(out);
        }
        break;
    case T_STAR:
        if ((IS_TEXT(a) || IS_LIST(a)) && IS_NUMERIC(b)) return repeat(I, n, a, numeric_value(b));
        if (IS_NUMERIC(a) && (IS_TEXT(b) || IS_LIST(b))) return repeat(I, n, b, numeric_value(a));
        break;
    case T_LT: case T_LE: case T_GT: case T_GE:
        if (IS_TEXT(a) && IS_TEXT(b)) {
            int c = compare_strings(text_of(a), text_of(b));
            return BOOL_VAL(op == T_LT ? c < 0 : op == T_LE ? c <= 0 : op == T_GT ? c > 0 : c >= 0);
        }
        break;
    default:
        break;
    }
    return error_at(I, n, "unsupported operand types for '%s': %s and %s", op_symbol(op), type_name(a), type_name(b));
}

/* A value entering a typed list or Map[K, V] (`holder` names it, `tag` is
 * the type it holds): returns a new reference to it (a plain whole number
 * becomes the integer type), or raises an error. */
Value lube_check_elem(Interp *I, const char *holder, int tag, Value v) {
    if (value_has_type(v, tag)) return retain(v);
    if (tag >= TT_INT8 && IS_NUM(v)) return int_from_whole(I, v.as.number, tag - TT_INT8);
    return lube_error(I, "this %s can only hold %s values, not %s", holder, tag_name(tag), a_type(v));
}

/* The error for a key an Object or Map does not take. */
static Value bad_key(Interp *I, Node *at, ObjMap *m, Value key) {
    if (m->dict)
        return error_at(I, at, "this %s has %s keys, not %s", type_name(OBJ_VAL(m)), tag_name(m->ktype), a_type(key));
    return error_at(I, at, "object keys must be str, Number or Bool values, not %s", a_type(key));
}

/* Arithmetic for C code (the standard library): same rules as the operators. */
Value lube_arith(Interp *I, TokenType op, Value a, Value b) { return binary_op(I, NULL, op, a, b); }

/* 1 if container holds item, 0 if not, -1 after raising an error. */
int lube_contains(Interp *I, Value c, Value x) {
    if (IS_LIST(c)) {
        ObjList *l = AS_LIST(c);
        for (size_t i = 0; i < l->count; i++)
            if (values_equal(l->items[i], x)) return 1;
        return 0;
    }
    if (IS_TEXT(c)) {
        if (!IS_TEXT(x)) {
            lube_error(I, "can only search text for text, not for %s", a_type(x));
            return -1;
        }
        ObjString *s = text_of(c), *sub = text_of(x);
        return str_find(s->chars, s->length, sub->chars, sub->length, 0) >= 0;
    }
    if (IS_MAP(c)) return map_key_ok(AS_MAP(c), x) && map_get(AS_MAP(c), x, NULL);
    if (IS_RANGE(c)) {
        if (!IS_NUMERIC(x)) return 0;
        ObjRange *r = AS_RANGE(c);
        double k = (numeric_value(x) - r->start) / r->step;
        return k >= 0 && k == floor(k) && k < range_length(r);
    }
    lube_error(I, "cannot look inside %s (expected a List, str, Object or Range)", a_type(c));
    return -1;
}

/* ======================================================================
 * Indexing and fields
 * ==================================================================== */

/* Checks an index (negative counts from the end). 1 = ok, 0 = error raised. */
static int list_index(Interp *I, Node *n, Value idx, size_t len, const char *what, size_t *out) {
    if (!IS_NUMERIC(idx)) {
        error_at(I, n, "%s index must be a Number, not %s", what, a_type(idx));
        return 0;
    }
    double d = numeric_value(idx);
    if (d != floor(d)) {
        error_at(I, n, "%s index must be a whole number, got %g", what, d);
        return 0;
    }
    double i = d < 0 ? d + (double)len : d;
    if (i < 0 || i >= (double)len) {
        char buf[64];
        format_number(buf, sizeof buf, d);
        error_at(I, n, "%s index %s is out of range (length %lu)", what, buf, (unsigned long)len);
        return 0;
    }
    *out = (size_t)i;
    return 1;
}

static Value index_get(Interp *I, Node *n, Value obj, Value idx) {
    size_t i;
    if (IS_LIST(obj)) {
        ObjList *l = AS_LIST(obj);
        return list_index(I, n, idx, l->count, "list", &i) ? retain(l->items[i]) : ERR_VAL;
    }
    if (IS_MAP(obj)) {
        ObjMap *m = AS_MAP(obj);
        Value v;
        if (m->dict && !map_key_ok(m, idx)) return bad_key(I, n, m, idx);
        if (map_key_ok(m, idx) && map_get(m, idx, &v)) return retain(v);
        ObjString *k = value_repr(idx);
        Value e = error_at(I, n, "the %s has no key %s (check with has() first, or use get(%s, key, default))",
                           m->dict ? "map" : "object", k->chars, m->dict ? "map" : "object");
        release(OBJ_VAL(k));
        return e;
    }
    if (IS_TEXT(obj)) {
        ObjString *s = text_of(obj);
        if (!list_index(I, n, idx, s->length, "string", &i)) return ERR_VAL;
        return retain(OBJ_VAL(I->chars[(unsigned char)s->chars[i]]));
    }
    if (IS_RANGE(obj)) {  /* in floating point: a range can be longer than any list */
        ObjRange *r = AS_RANGE(obj);
        double count = range_length(r);
        double d = IS_NUMERIC(idx) ? numeric_value(idx) : 0.5;
        if (d != floor(d)) return error_at(I, n, "range index must be a whole number");
        double k = d < 0 ? d + count : d;
        if (!(k >= 0 && k < count)) return error_at(I, n, "range index %g is out of range (length %g)", d, count);
        return NUM_VAL(r->start + k * r->step);
    }
    return error_at(I, n, "cannot index into %s", a_type(obj));
}

static Value field_get(Interp *I, Node *n, Value obj, ObjString *name) {
    if (IS_STROBJ(obj)) {
        I->frame->line = n->line;
        return lube_string_property(I, obj, name);
    }
    if (IS_STRING(obj))
        return error_at(I, n, "a str has no properties (it is plain text): use len(x) for its length, or "
                              "String(x).%s", name->chars);
    if (IS_MAP(obj)) {
        ObjMap *m = AS_MAP(obj);
        Value v;
        if (!map_key_ok(m, OBJ_VAL(name))) return bad_key(I, n, m, OBJ_VAL(name));
        if (map_get(m, OBJ_VAL(name), &v)) return retain(v);
        if (m->klass) return error_at(I, n, "%s has no field '%s'", a_type(obj), name->chars);
        return error_at(I, n, "the %s has no field '%s' (check with has() first, or use get(%s, key, default))",
                        m->dict ? "map" : "object", name->chars, m->dict ? "map" : "object");
    }
    Value f;
    if (map_get(I->globals, OBJ_VAL(name), &f) && IS_CALLABLE(f))
        return error_at(I, n, "%s has no field '%s' (to call the function, write .%s())", a_type(obj),
                        name->chars, name->chars);
    return error_at(I, n, "%s has no field '%s'", a_type(obj), name->chars);
}

/* ======================================================================
 * Places: the target of an assignment or of `&`, such as x, x[i] or x.a[2].
 * The path's keys are evaluated once; every later step only walks data and
 * runs no user code, so pointers into containers stay valid while in use.
 * ==================================================================== */

#define MAX_PATH 16

typedef struct {
    Node *root;                     /* the N_VAR the path starts from */
    Value keys[MAX_PATH];
    unsigned char is_field[MAX_PATH];
    int nkeys;
} Place;

static void place_free(Place *p) {
    for (int i = 0; i < p->nkeys; i++) release(p->keys[i]);
    p->nkeys = 0;
}

static int is_place_node(Node *n) {
    while (n->kind == N_INDEX || n->kind == N_FIELD) n = n->kind == N_INDEX ? n->as.index.object : n->as.field.object;
    return n->kind == N_VAR;
}

/* Evaluates the keys of a place. 0 = ok, -1 = error raised. */
static int eval_place_keys(Interp *I, Node *n, Place *p);

static int eval_place(Interp *I, Node *n, Place *p) {
    int len = 0;
    Node *m = n;
    for (; m->kind == N_INDEX || m->kind == N_FIELD; m = m->kind == N_INDEX ? m->as.index.object : m->as.field.object)
        len++;
    if (m->kind != N_VAR) {  /* LCOV_EXCL_START: the parser guarantees this; checked rather than trusted */
        error_at(I, n, "can only change a variable, or an index or field of one");
        return -1;
    }  /* LCOV_EXCL_STOP */
    if (len > MAX_PATH) {  /* checked up front, so the recursion below stays shallow */
        error_at(I, n, "this is nested too deeply to change in one step");
        return -1;
    }
    return eval_place_keys(I, n, p);
}

static int eval_place_keys(Interp *I, Node *n, Place *p) {
    if (n->kind == N_VAR) {
        p->root = n;
        p->nkeys = 0;
        return 0;
    }
    int is_field = n->kind == N_FIELD;
    if (eval_place_keys(I, is_field ? n->as.field.object : n->as.index.object, p) < 0) return -1;
    Value key;
    if (is_field) {
        key = retain(OBJ_VAL(n->as.field.name));
    } else {
        key = eval_value(I, n->as.index.index);
        if (IS_ERR(key)) {
            place_free(p);
            return -1;
        }
    }
    p->keys[p->nkeys] = key;
    p->is_field[p->nkeys] = (unsigned char)is_field;
    p->nkeys++;
    return 0;
}

/* May this code change the place's root variable? 0 = yes, -1 = error. */
LUBE_NOINLINE static int check_changeable(Interp *I, Node *at, Place *p) {
    VarRef *r = &p->root->as.var;
    Origin origin;
    switch (r->kind) {
    case VAR_LOCAL:
        if (r->changeable && !(r->origin == ORIGIN_THIS && I->frame->this_locked)) return 0;
        origin = r->origin;
        break;
    case VAR_GLOBAL:
        if (!global_origin(I, r->name, &origin)) return 0;
        break;
    case VAR_CAPTURE:
        origin = ORIGIN_CAPTURE;
        break;
    default:
        origin = ORIGIN_SELF;
        break;
    }
    cannot_change(I, at, r->name->chars, origin);
    return -1;
}

/* Like check_changeable, without raising an error. */
static int place_changeable(Interp *I, Place *p) {
    VarRef *r = &p->root->as.var;
    Origin origin;
    if (r->kind == VAR_LOCAL) return r->changeable && !(r->origin == ORIGIN_THIS && I->frame->this_locked);
    return r->kind == VAR_GLOBAL && !global_origin(I, r->name, &origin);
}

static Value *root_slot(Interp *I, Node *at, Place *p) {
    VarRef *r = &p->root->as.var;
    if (r->kind == VAR_LOCAL) return &SLOT(r->index);
    Value *g = map_find(I->globals, OBJ_VAL(r->name));
    if (!g) {
        error_at(I, at, "undefined variable '%s' (declare it first with 'var %s = ...')", r->name->chars,
                 r->name->chars);
        return NULL;
    }
    if (g->type == VAL_LENT) {
        error_at(I, at, IN_USE, r->name->chars);
        return NULL;
    }
    return g;
}

/* Walks the path from *slot, giving each container along the way a single
 * owner first (copy-on-write). Returns the final slot, or NULL on error.
 * With `made` (for `=`), a missing last map key is created holding nothing,
 * and *made is set to that map so a failed assignment can remove the key
 * again (no "nothing" may stay behind in a value). */
static Value *navigate(Interp *I, Node *at, Value *slot, Place *p, ObjMap **made, int *elem, const char **holder,
                       const TypeDesc **ftype) {
    *elem = TT_ANY;  /* the type the container holding the final slot accepts... */
    *holder = NULL;  /* ...and that container's type name, for messages */
    *ftype = NULL;   /* ...or, for a field of an instance, that field's declared type */
    for (int i = 0; i < p->nkeys; i++) {
        Value key = p->keys[i];
        if (p->is_field[i] && !IS_MAP(*slot)) {
            if (IS_STROBJ(*slot))
                error_at(I, at, "the properties of a String are read-only");
            else
                error_at(I, at, "cannot set field '%s' on %s (only objects have fields)", AS_STRING(key)->chars,
                         a_type(*slot));
            return NULL;
        }
        if (IS_LIST(*slot)) {
            size_t k;
            if (!list_index(I, at, key, AS_LIST(*slot)->count, "list", &k)) return NULL;
            make_unique(slot);
            *elem = AS_LIST(*slot)->elem;
            *holder = type_name(*slot);
            slot = &AS_LIST(*slot)->items[k];
        } else if (IS_MAP(*slot)) {
            if (!map_key_ok(AS_MAP(*slot), key)) {
                bad_key(I, at, AS_MAP(*slot), key);
                return NULL;
            }
            make_unique(slot);
            *elem = AS_MAP(*slot)->vtype;
            *holder = type_name(*slot);
            ObjMap *m = AS_MAP(*slot);
            if (m->klass) {  /* an instance: only its declared fields exist */
                ObjClass *c = m->klass;
                int f = -1;
                for (int j = 0; j < c->nfields && f < 0 && IS_STRING(key); j++)
                    if (keys_same(OBJ_VAL(c->field_names[j]), key)) f = j;
                if (f < 0) {
                    if (IS_STRING(key)) error_at(I, at, "%s has no field '%s'", with_article(c->name->chars), AS_STRING(key)->chars);
                    else error_at(I, at, "%s has fields named by text, not %s", with_article(c->name->chars), a_type(key));
                    return NULL;
                }
                *ftype = &c->field_types[f];
            }
            Value *child = map_find(m, key);
            if (!child) {
                if (i < p->nkeys - 1 || !made) {
                    ObjString *k = value_repr(key);
                    const char *what = m->dict ? "map" : "object";
                    if (p->is_field[i])
                        error_at(I, at, "the %s has no field '%s' (add it first with an assignment)", what,
                                 AS_STRING(key)->chars);
                    else
                        error_at(I, at, "the %s has no key %s (add it first with an assignment)", what, k->chars);
                    release(OBJ_VAL(k));
                    return NULL;
                }
                map_set(m, retain(key), NIL_VAL);
                child = map_find(m, key);
                *made = m;
            }
            slot = child;
        } else if (IS_TEXT(*slot)) {
            error_at(I, at, "text cannot be changed in place; build new text instead");
            return NULL;
        } else {
            error_at(I, at, "cannot change an element of %s", a_type(*slot));
            return NULL;
        }
    }
    return slot;
}

/* Lends a place: moves its value out (into *out) and leaves the root empty
 * until place_restore. When the path has keys, the root value is kept in
 * *holder, privately, with a hole where the lent part was. */
LUBE_NOINLINE static int place_take(Interp *I, Node *at, Place *p, Value *holder, Value *out) {
    Value *root = root_slot(I, at, p);
    if (!root) return -1;
    Value empty = p->root->as.var.kind == VAR_GLOBAL ? LENT_VAL : NIL_VAL;
    int elem;
    const char *container;
    const TypeDesc *ftype;
    if (p->nkeys == 0) {
        *out = *root;
        *root = empty;
        *holder = NIL_VAL;
        return 0;
    }
    Value h = *root;  /* the root's reference moves into h */
    Value *slot = navigate(I, at, &h, p, NULL, &elem, &container, &ftype);  /* never lend a missing key */
    if (!slot) {
        *root = h;
        return -1;
    }
    *out = *slot;  /* moved, not copied: the callee changes it in place */
    *slot = NIL_VAL;
    *root = empty;
    *holder = h;
    return 0;
}

/* Puts a lent value back. It still has its type: the callee's parameter
 * kept the type it was given. The path cannot fail either, because the
 * holder was private while the value was out. */
LUBE_NOINLINE static void place_restore(Interp *I, Place *p, Value holder, Value value) {
    VarRef *r = &p->root->as.var;
    Value *root = r->kind == VAR_LOCAL ? &SLOT(r->index) : map_find(I->globals, OBJ_VAL(r->name));
    if (p->nkeys == 0) {
        release(*root);
        *root = value;
        return;
    }
    Value *slot = &holder;
    for (int i = 0; i < p->nkeys; i++) {
        Value key = p->keys[i];
        if (IS_LIST(*slot)) {
            ObjList *l = AS_LIST(*slot);
            double d = numeric_value(key);  /* the key may be a plain number or a sized integer */
            if (d < 0) d += (double)l->count;
            slot = &l->items[(size_t)d];
        } else {
            slot = map_find(AS_MAP(*slot), key);
        }
    }
    release(*slot);
    *slot = value;
    release(*root);
    *root = holder;
}

/* Reads a place without changing anything. */
static Value place_get(Interp *I, Node *at, Place *p) {
    Value v = get_var(I, p->root, &p->root->as.var);
    for (int i = 0; i < p->nkeys && !IS_ERR(v); i++) {
        Value next = p->is_field[i] ? field_get(I, at, v, AS_STRING(p->keys[i])) : index_get(I, at, v, p->keys[i]);
        release(v);
        v = next;
    }
    return v;
}

/* ======================================================================
 * Calls
 * ==================================================================== */

LUBE_COLD static Value arity_error(Interp *I, const char *name, int min, int max, int got) {
    if (min == max)
        return lube_error(I, "%s() takes %d argument%s but got %d", name, min, min == 1 ? "" : "s", got);
    if (max < 0)
        return lube_error(I, "%s() takes at least %d argument%s but got %d", name, min, min == 1 ? "" : "s", got);
    return lube_error(I, "%s() takes %d to %d arguments but got %d", name, min, max, got);
}

static const char *callee_name(Value f) {
    if (IS_NATIVE(f)) return AS_NATIVE(f)->name;
    if (IS_CLASS(f)) return AS_CLASS(f)->name->chars;
    FuncDecl *d = AS_FUNCTION(f)->decl;
    if (d->owner && !strcmp(d->name->chars, "constructor")) return d->owner->name->chars;  /* Dog(...) */
    return d->name ? d->name->chars : "<lambda>";
}

/* The declared type a written type stands for. A class is looked up by
 * name when the function runs (classes are globals that never change). */
static int ann_desc(Interp *I, const TypeAnn *t, TypeDesc *out) {
    *out = t->d;
    if (t->d.kind != TD_CLASS) return 1;
    ObjString *name = t->cls->as.var.name;
    Value *c = map_find(I->globals, OBJ_VAL(name));
    if (!c || !IS_CLASS(*c)) {
        lube_error(I, "'%s' is not a type or a class", name->chars);
        return 0;
    }
    out->cls = AS_CLASS(*c);
    return 1;
}

/* Checks *v, the value of parameter i (in stack slot `slot`), against the
 * parameter's declared type; a plain whole number becomes the integer type
 * it asks for (not through &, which must not change the caller's type).
 * Every parameter keeps a type: a typed var parameter the declared one,
 * the others their value's. Returns 0 after raising an error. */
static int check_param(Interp *I, FuncDecl *d, const char *name, int i, Value *v, int slot) {
    I->decl[slot] = desc_of(*v);
    if (!d->types || !d->types[i].set) return 1;
    TypeDesc want;
    Value out;
    if (!ann_desc(I, &d->types[i], &want)) return 0;
    if (!desc_accepts(want, *v, &out) || (d->modes[i] == PARAM_REF && out.type != v->type)) {
        lube_error(I, "parameter '%s' of %s() is %s, so it cannot take %s", d->params[i]->chars, name,
                   with_article(desc_name(want)), a_type(*v));
        return 0;
    }
    *v = out;
    if (d->modes[i] == PARAM_VAR) I->decl[slot] = want;
    return 1;
}

/* Checks a function's result against its declared return type (a plain
 * whole number becomes the integer type asked for). In #strict code, a
 * function without a return type returns nothing. */
static Value check_return(Interp *I, FuncDecl *d, const char *name, Value result) {
    Value out;
    TypeDesc want;
    if (!d->ret.set) {
        if (IS_NIL(result)) return result;
        out = lube_error(I, "#strict: %s() returns %s, but it declares no return type (add one, like ): %s)", name,
                         a_type(result), type_name(result));
    } else if (!ann_desc(I, &d->ret, &want)) {
        out = ERR_VAL;
    } else if (IS_NIL(result)) {
        return lube_error(I, "%s() must return %s, but it ended without returning a value", name,
                          with_article(desc_name(want)));
    } else if (desc_accepts(want, result, &out)) {
        return out;
    } else {
        out = lube_error(I, "%s() must return %s, not %s", name, with_article(desc_name(want)), a_type(result));
    }
    release(result);
    return out;
}

/* Runs a function whose arguments are in stack[base .. base+argc). The
 * argument slots are left for the caller to release (lent values are moved
 * back from them); everything else in the frame is released here. */
static Value call_function(Interp *I, ObjFunction *fn, int argc, int base) {
    FuncDecl *d = fn->decl;
    const char *name = callee_name(OBJ_VAL(fn));
    if (argc > d->nparams) {
        int required = d->nparams;
        while (required > 0 && d->defaults[required - 1]) required--;
        return arity_error(I, name, required, d->nparams, argc);
    }
    for (int i = argc; i < d->nparams; i++)
        if (!d->defaults[i]) return lube_error(I, "%s() is missing argument '%s'", name, d->params[i]->chars);
    for (int i = 0; i < argc; i++)
        if (!check_param(I, d, name, i, &I->stack[base + i], base + i)) return ERR_VAL;
    if (!lube_stack_ok(I)) return lube_error(I, "stack overflow: too much recursion");
    int extra = d->nslots - argc;
    if (extra > 0) {
        if (!stack_room(I, extra)) return lube_error(I, "stack overflow: too much recursion");
        for (int i = 0; i < extra; i++) I->stack[I->sp + i] = NIL_VAL;
        I->sp += extra;
    }

    Frame frame;
    frame.prev = I->frame;
    frame.name = name;
    frame.src = d->src;
    frame.line = d->line;
    frame.base = base;
    frame.fn = fn;
    frame.this_locked = I->this_locked_next;
    I->this_locked_next = 0;
    I->frame = &frame;

    Value result = NIL_VAL;
    for (int i = argc; i < d->nparams; i++) {
        Value v = eval_value(I, d->defaults[i]);
        if (IS_ERR(v)) {
            result = v;
            goto done;
        }
        if (!check_param(I, d, name, i, &v, base + i)) {
            release(v);
            result = ERR_VAL;
            goto done;
        }
        set_slot(I, i, v);
    }
    if (d->is_arrow) {
        result = eval(I, d->body);
    } else {
        Exec r = exec_stmts(I, &d->body->as.block.stmts);
        if (r == EX_ERROR) {
            result = ERR_VAL;
        } else if (r == EX_RETURN) {
            result = I->ret_val;
            I->ret_val = NIL_VAL;
        }
    }
    if (!IS_ERR(result) && (d->ret.set || d->strict)) result = check_return(I, d, name, result);
done:
    if (IS_ERR(result)) trace_frame(I, &frame);
    pop_to(I, base + argc);
    I->frame = frame.prev;
    return result;
}

/* Dog(args): a new instance with every field at its starting value, then the
 * constructor (this class's or the nearest parent's) runs with `this` lent
 * to it. The arguments are in stack[base .. base+argc). */
static Value construct(Interp *I, ObjClass *cls, int argc, int base) {
    ObjClass *owner = cls;
    while (owner && IS_NIL(owner->constructor)) owner = owner->parent;
    FuncDecl *d = owner ? AS_FUNCTION(owner->constructor)->decl : NULL;
    int max = d ? d->nparams - 1 : 0, min = max;
    while (d && min > 0 && d->defaults[min]) min--;
    if (argc < min || argc > max) return arity_error(I, cls->name->chars, min, max, argc);
    if (!stack_room(I, 1)) return lube_error(I, "stack overflow: too much recursion");

    ObjMap *inst = new_map();
    inst->klass = cls;
    for (int i = 0; i < cls->nfields; i++)
        map_set(inst, retain(OBJ_VAL(cls->field_names[i])), retain(cls->field_defaults[i]));
    if (!d) return OBJ_VAL(inst);

    /* `this` goes in front of the arguments for the call, then out again */
    memmove(&I->stack[base + 1], &I->stack[base], sizeof(Value) * (size_t)argc);
    I->stack[base] = OBJ_VAL(inst);
    I->sp++;
    Value r = call_function(I, AS_FUNCTION(owner->constructor), argc + 1, base);
    Value made = I->stack[base];
    memmove(&I->stack[base], &I->stack[base + 1], sizeof(Value) * (size_t)argc);
    I->stack[base + argc] = NIL_VAL;
    I->sp--;
    if (IS_ERR(r)) {
        release(made);
        return r;
    }
    release(r);  /* whatever the constructor returned is not used */
    return made;
}

static Value call_value(Interp *I, Value callee, int argc, int base) {
    if (IS_FUNCTION(callee)) return call_function(I, AS_FUNCTION(callee), argc, base);
    if (IS_CLASS(callee)) return construct(I, AS_CLASS(callee), argc, base);
    if (IS_NATIVE(callee)) {
        ObjNative *nf = AS_NATIVE(callee);
        if (argc < nf->min_args || (nf->max_args >= 0 && argc > nf->max_args))
            return arity_error(I, nf->name, nf->min_args, nf->max_args, argc);
        return nf->fn(I, argc, &I->stack[base]);
    }
    return lube_error(I, "cannot call %s", a_type(callee)); /* LCOV_EXCL_LINE: callers check first */
}

/* Calls a function from C code (the standard library). Borrows argv. */
Value lube_call(Interp *I, Value fn, int argc, Value *argv) {
    if (IS_FUNCTION(fn)) {
        FuncDecl *d = AS_FUNCTION(fn)->decl;
        for (int i = 0; i < d->nparams; i++)
            if (d->modes[i] == PARAM_REF)
                return lube_error(I, "%s() changes its argument '%s' with &, so it cannot be used as a callback",
                                  callee_name(fn), d->params[i]->chars);
    } else if (IS_NATIVE(fn) && AS_NATIVE(fn)->changes_first) {
        return lube_error(I, "%s() changes its argument, so it cannot be used as a callback", callee_name(fn));
    }
    if (!stack_room(I, argc)) return lube_error(I, "stack overflow: too much recursion");
    int base = I->sp;
    for (int i = 0; i < argc; i++) I->stack[base + i] = retain(argv[i]);
    I->sp = base + argc;
    Value r = call_value(I, fn, argc, base);
    pop_to(I, base);
    return r;
}

typedef struct {
    int pos;                        /* argument position */
    Place place;
    Value holder;
    int lent;
} Loan;

static int lent_at(Loan *loans, int n, int pos) {
    for (int j = 0; j < n; j++)
        if (loans[j].pos == pos) return 1;
    return 0;
}

/* `&` must be used exactly for the parameters that change. */
LUBE_NOINLINE static int check_loans(Interp *I, Node *at, Value callee, int argc, Loan *loans, int nloans, int this_by_value) {
    const char *name = callee_name(callee);
    if (IS_CLASS(callee)) {
        if (nloans == 0) return 0;
        error_at(I, at, "%s() does not take & arguments", name);
        return -1;
    }
    if (IS_NATIVE(callee)) {
        ObjNative *nf = AS_NATIVE(callee);
        for (int j = 0; j < nloans; j++) {
            if (!(nf->changes_first && loans[j].pos == 0)) {
                error_at(I, at, "%s() does not change argument %d, so it does not take '&'", name, loans[j].pos + 1);
                return -1;
            }
        }
        if (nf->changes_first && argc > 0 && !lent_at(loans, nloans, 0)) {
            error_at(I, at, "%s() changes its first argument, so pass it with &: %s(&x, ...) or x.%s(...)", name, name,
                     name);
            return -1;
        }
        return 0;
    }
    FuncDecl *d = AS_FUNCTION(callee)->decl;
    for (int j = 0; j < nloans; j++) {
        int pos = loans[j].pos;
        if (pos < d->nparams && d->modes[pos] != PARAM_REF) {
            error_at(I, at, "%s() does not change '%s', so it does not take '&'", name, d->params[pos]->chars);
            return -1;
        }
    }
    for (int i = this_by_value ? 1 : 0; i < argc && i < d->nparams; i++) {
        if (d->modes[i] == PARAM_REF && !lent_at(loans, nloans, i)) {
            error_at(I, at, "%s() changes '%s', so pass it with &: %s(&x)", name, d->params[i]->chars, name);
            return -1;
        }
    }
    return 0;
}

static int same_root(Place *a, Place *b) {
    VarRef *x = &a->root->as.var, *y = &b->root->as.var;
    if (x->kind != y->kind) return 0;
    return x->kind == VAR_LOCAL ? x->index == y->index : x->name == y->name;
}

/* Evaluates arguments, lends `&` places, calls, and returns lent values.
 * Takes over `callee`, `self` (when has_self and no self_place) and
 * `self_place` (a receiver lent to a method like push). */
static Value invoke(Interp *I, Node *at, Value callee, int has_self, Value self, Place *self_place, NodeList *args,
                    int this_by_value) {
    int extra = has_self ? 1 : 0;
    int argc = args->count + extra;
    int nloans = self_place ? 1 : 0;
    for (int i = 0; i < args->count; i++)
        if (args->items[i]->kind == N_REF) nloans++;
    if (!stack_room(I, argc)) {  /* LCOV_EXCL_START: the C stack limit is always reached first */
        release(self);
        if (self_place) place_free(self_place);
        release(callee);
        return error_at(I, at, "stack overflow: too much recursion");
    }  /* LCOV_EXCL_STOP */
    int base = I->sp;
    for (int i = 0; i < argc; i++) I->stack[base + i] = NIL_VAL;
    I->sp = base + argc;

    Loan *loans = nloans ? xmalloc(sizeof(Loan) * (size_t)nloans) : NULL;
    int k = 0;
    Value result = ERR_VAL;
    if (self_place) {
        loans[0].pos = 0;
        loans[0].place = *self_place;
        loans[0].holder = NIL_VAL;
        loans[0].lent = 0;
        k = 1;
    } else if (has_self) {
        I->stack[base] = self;
    }

    for (int i = 0; i < args->count; i++) {
        Node *a = args->items[i];
        int pos = i + extra;
        if (a->kind == N_REF) {
            if (eval_place(I, a->as.expr, &loans[k].place) < 0) goto finish;
            loans[k].pos = pos;
            loans[k].holder = NIL_VAL;
            loans[k].lent = 0;
            k++;
        } else {
            Value v = eval_value(I, a);
            if (IS_ERR(v)) goto finish;
            I->stack[base + pos] = v;
        }
    }
    if (check_loans(I, at, callee, argc, loans, k, this_by_value) < 0) goto finish;
    for (int j = 0; j < k; j++) {
        for (int m = 0; m < j; m++) {
            if (same_root(&loans[m].place, &loans[j].place)) {
                error_at(I, at, "'%s' is passed with & more than once in this call",
                         loans[j].place.root->as.var.name->chars);
                goto finish;
            }
        }
        if (check_changeable(I, at, &loans[j].place) < 0) goto finish;
        Value v;
        if (place_take(I, at, &loans[j].place, &loans[j].holder, &v) < 0) goto finish;
        loans[j].lent = 1;
        I->stack[base + loans[j].pos] = v;
    }
    if (I->frame) I->frame->line = at->line;
    if (this_by_value && IS_FUNCTION(callee)) I->this_locked_next = 1;  /* read by call_function at once */
    result = call_value(I, callee, argc, base);

finish:
    for (int j = k - 1; j >= 0; j--) {  /* return what was lent, even after an error */
        if (loans[j].lent) {
            Value v = I->stack[base + loans[j].pos];
            I->stack[base + loans[j].pos] = NIL_VAL;
            place_restore(I, &loans[j].place, loans[j].holder, v);
        }
        place_free(&loans[j].place);
    }
    free(loans);
    pop_to(I, base);
    release(callee);
    return result;
}

static int takes_first_by_ref(Value f) {
    if (IS_NATIVE(f)) return AS_NATIVE(f)->changes_first;
    if (IS_CLASS(f)) return 0;  /* x.Point() is Point(x) */
    FuncDecl *d = AS_FUNCTION(f)->decl;
    return d->nparams > 0 && d->modes[0] == PARAM_REF;
}

/* x.name(args): a function stored in map x under `name` is called as is;
 * otherwise the variable `name` is called as name(x, args). If that function
 * changes its first argument (like push), x is passed as &x. */
LUBE_NOINLINE static Value eval_method(Interp *I, Node *n) {
    Node *recv = n->as.method.object;
    ObjString *name = n->as.method.name;
    Place place;
    int have_place = 0;
    Value obj;
    if (is_place_node(recv)) {
        if (eval_place(I, recv, &place) < 0) return ERR_VAL;
        have_place = 1;
        obj = place_get(I, n, &place);
    } else {
        obj = eval_value(I, recv);
    }
    if (IS_ERR(obj)) {
        if (have_place) place_free(&place);
        return obj;
    }

    Value own, callee;
    if (IS_INSTANCE(obj)) {  /* a method: lend `this` if it can change, or pass it read-only */
        Value m = find_method(AS_MAP(obj)->klass, name);
        if (!IS_NIL(m)) {
            callee = retain(m);
            if (have_place && place_changeable(I, &place)) {
                release(obj);
                return invoke(I, n, callee, 1, NIL_VAL, &place, &n->as.method.args, 0);
            }
            if (have_place) place_free(&place);
            return invoke(I, n, callee, 1, obj, NULL, &n->as.method.args, 1);
        }
    }
    if (IS_MAP(obj) && map_get(AS_MAP(obj), OBJ_VAL(name), &own)) {
        callee = retain(own);
        release(obj);
        if (have_place) place_free(&place);
        if (!IS_CALLABLE(callee)) {
            Value e = error_at(I, n, "'%s' is not a function (it is %s)", name->chars, a_type(callee));
            release(callee);
            return e;
        }
        return invoke(I, n, callee, 0, NIL_VAL, NULL, &n->as.method.args, 0);
    }

    VarRef *r = &n->as.method.fallback;
    if (r->kind == VAR_GLOBAL && !map_find(I->globals, OBJ_VAL(name))) {
        Value e = error_at(I, n, "%s has no method '%s'", type_name(obj), name->chars);
        release(obj);
        if (have_place) place_free(&place);
        return e;
    }
    callee = get_var(I, n, r);
    if (!IS_ERR(callee) && !IS_CALLABLE(callee) && !IS_CLASS(callee)) {
        error_at(I, n, "'%s' is not a function (it is %s)", name->chars, a_type(callee));
        release(callee);
        callee = ERR_VAL;
    }
    if (IS_ERR(callee)) {
        release(obj);
        if (have_place) place_free(&place);
        return callee;
    }
    if (takes_first_by_ref(callee)) {
        release(obj);  /* drop our reference so the value can change in place */
        if (!have_place) {
            Value e = error_at(I, n, "%s() changes its first argument, so call it on a variable (or a field or index of one)",
                               callee_name(callee));
            release(callee);
            return e;
        }
        return invoke(I, n, callee, 1, NIL_VAL, &place, &n->as.method.args, 0);
    }
    if (have_place) place_free(&place);
    return invoke(I, n, callee, 1, obj, NULL, &n->as.method.args, 0);
}

/* ======================================================================
 * Expressions
 * ==================================================================== */

LUBE_NOINLINE static Value eval_assign(Interp *I, Node *n) {
    Place p;
    if (eval_place(I, n->as.assign.target, &p) < 0) return ERR_VAL;
    Value rhs = eval_value(I, n->as.assign.value);
    if (IS_ERR(rhs)) {
        place_free(&p);
        return rhs;
    }
    I->frame->line = n->line;
    Value *slot = NULL;
    int elem = TT_ANY;
    const char *holder = NULL;
    const TypeDesc *ftype = NULL;
    int whole = p.nkeys == 0;  /* assigning the variable itself */
    Value field = whole ? NIL_VAL : p.keys[p.nkeys - 1];
    ObjMap *made = NULL;  /* the map that got a new, still empty key */
    if (check_changeable(I, n, &p) == 0) {
        Value *root = root_slot(I, n, &p);
        if (root) slot = navigate(I, n, root, &p, n->as.assign.op == T_ASSIGN ? &made : NULL, &elem, &holder, &ftype);
    }
    if (!slot) {
        place_free(&p);
        release(rhs);
        return ERR_VAL;
    }
    /* the declared type the new value must have: a var's, or a class field's */
#define KEEP_TYPE(v)                                                                          \
    do {                                                                                      \
        if (whole) v = keep_type(I, n, &p.root->as.var, v);                                   \
        else if (ftype) v = keep_field_type(I, n, ftype, field, holder, v);                   \
    } while (0)

    TokenType op = n->as.assign.op;
    if (op == T_ASSIGN) {
        if (elem != TT_ANY) {  /* into a typed list or map */
            Value c = lube_check_elem(I, holder, elem, rhs);
            release(rhs);
            rhs = c;
        }
        if (!IS_ERR(rhs)) KEEP_TYPE(rhs);
        if (IS_ERR(rhs) && made) map_delete(made, field, NULL);  /* the new key was refused */
        place_free(&p);
        if (IS_ERR(rhs)) return rhs;
        Value old = *slot;
        *slot = rhs;
        release(old);
        return retain(rhs);
    }
    TokenType bin = compound_base(op);
    /* `+=` on a string or list nobody else shares: append in place */
    if (bin == T_PLUS && IS_OBJ(*slot) && slot->as.obj->rc == 1) {
        if (IS_STRING(*slot)) {
            ObjString *add = value_to_string(rhs);
            *slot = OBJ_VAL(string_append(AS_STRING(*slot), add->chars, add->length));
            release(OBJ_VAL(add));
            release(rhs);
            place_free(&p);
            return retain(*slot);
        }
        if (IS_LIST(*slot) && IS_LIST(rhs)) {
            ObjList *l = AS_LIST(*slot), *add = AS_LIST(rhs);
            for (size_t i = 0; i < add->count && l->elem != TT_ANY; i++) {  /* check all before changing */
                Value c = lube_check_elem(I, type_name(*slot), l->elem, add->items[i]);
                if (IS_ERR(c)) {
                    release(rhs);
                    place_free(&p);
                    return c;
                }
                release(c);
            }
            list_reserve(l, l->count + add->count);
            for (size_t i = 0; i < add->count; i++) {
                Value c = lube_check_elem(I, type_name(*slot), l->elem, add->items[i]);  /* checked above */
                l->items[l->count++] = c;
            }
            release(rhs);
            place_free(&p);
            return retain(*slot);
        }
    }
    Value r = binary_op(I, n, bin, *slot, rhs);
    release(rhs);
    if (!IS_ERR(r) && elem != TT_ANY) {
        Value c = lube_check_elem(I, holder, elem, r);
        release(r);
        r = c;
    }
    if (!IS_ERR(r)) KEEP_TYPE(r);
#undef KEEP_TYPE
    place_free(&p);
    if (IS_ERR(r)) return r;
    Value old = *slot;
    *slot = r;
    release(old);
    return retain(r);
}

/* Lube has no null. A call to a function that returns nothing yields no
 * value at all (internally NIL_VAL). It may be used as a statement, but
 * wherever a value is needed, eval_value() turns it into an error, so
 * "nothing" can never be stored, passed, compared or printed. */
LUBE_COLD static Value no_value(Interp *I, Node *n) {
    const char *name = NULL;
    if (n->kind == N_CALL && n->as.call.callee->kind == N_VAR) name = n->as.call.callee->as.var.name->chars;
    else if (n->kind == N_METHOD) name = n->as.method.name->chars;
    if (name) return error_at(I, n, "%s() does not return a value, so there is nothing to use here", name);
    return error_at(I, n, "this function call does not return a value, so there is nothing to use here");
}

static Value eval_value(Interp *I, Node *n) {
    Value v = eval(I, n);
    return IS_NIL(v) ? no_value(I, n) : v;
}

static Value make_closure(Interp *I, FuncDecl *d) {
    ObjFunction *f = new_function(d, d->ncaptures);
    for (int i = 0; i < d->ncaptures; i++) {
        CaptureSrc c = d->captures[i];
        Value v = c.kind == VAR_LOCAL     ? SLOT(c.index)
                  : c.kind == VAR_CAPTURE ? I->frame->fn->captures[c.index]
                                          : OBJ_VAL(I->frame->fn);
        f->captures[i] = retain(v);  /* a copy: later changes outside are not seen */
    }
    return OBJ_VAL(f);
}

static Value eval(Interp *I, Node *n) {
    if (!lube_stack_ok(I)) return error_at(I, n, "stack overflow: too much recursion");
    switch (n->kind) {
    case N_LITERAL:
        return retain(n->as.literal);

    case N_INTERP: {
        StrBuf sb;
        sb_init(&sb);
        for (int i = 0; i < n->as.items.count; i++) {
            Value v = eval_value(I, n->as.items.items[i]);
            if (IS_ERR(v)) {
                sb_free(&sb);
                return v;
            }
            append_value(&sb, v, 0);
            release(v);
        }
        ObjString *s = new_string(sb.data ? sb.data : "", sb.len);
        sb_free(&sb);
        return OBJ_VAL(s);
    }

    case N_LIST: {
        ObjList *l = new_list();
        list_reserve(l, (size_t)n->as.items.count);
        for (int i = 0; i < n->as.items.count; i++) {
            Value v = eval_value(I, n->as.items.items[i]);
            if (IS_ERR(v)) {
                release(OBJ_VAL(l));
                return v;
            }
            list_push(l, v);
        }
        return OBJ_VAL(l);
    }

    case N_TYPED_LIST: {
        ObjList *l = new_list();
        l->elem = n->as.tlist.elem;
        list_reserve(l, (size_t)n->as.tlist.items.count);
        for (int i = 0; i < n->as.tlist.items.count; i++) {
            Node *item = n->as.tlist.items.items[i];
            Value v = eval_value(I, item);
            if (IS_ERR(v)) {
                release(OBJ_VAL(l));
                return v;
            }
            I->frame->line = item->line;
            Value c = lube_check_elem(I, type_name(OBJ_VAL(l)), l->elem, v);
            release(v);
            if (IS_ERR(c)) {
                release(OBJ_VAL(l));
                return c;
            }
            list_push(l, c);
        }
        return OBJ_VAL(l);
    }

    case N_TYPED_MAP: {
        ObjMap *m = new_map();
        m->dict = 1;
        m->ktype = (unsigned char)n->as.tmap.ktype;
        m->vtype = (unsigned char)n->as.tmap.vtype;
        return OBJ_VAL(m);
    }

    case N_MAP: {
        ObjMap *m = new_map();
        for (int i = 0; i < n->as.map.keys.count; i++) {
            Value k = eval_value(I, n->as.map.keys.items[i]);
            if (IS_ERR(k)) {
                release(OBJ_VAL(m));
                return k;
            }
            if (!is_key(k)) {
                Value e = error_at(I, n, "object keys must be str, Number or Bool values, not %s", a_type(k));
                release(k);
                release(OBJ_VAL(m));
                return e;
            }
            Value v = eval_value(I, n->as.map.values.items[i]);
            if (IS_ERR(v)) {
                release(k);
                release(OBJ_VAL(m));
                return v;
            }
            map_set(m, k, v);
        }
        return OBJ_VAL(m);
    }

    case N_VAR: {
        Value v = get_var(I, n, &n->as.var);
        if (!IS_CLASS(v)) return v;
        Value made = construct(I, AS_CLASS(v), 0, I->sp);  /* Dog alone is Dog() */
        release(v);
        return made;
    }

    case N_SUPER: {  /* the parent class's method (or constructor), with the same `this` */
        ObjClass *parent = I->frame->fn->decl->owner->parent;
        ObjString *name = n->as.super_call.name;
        Value callee = NIL_VAL;
        if (name) {
            callee = parent ? find_method(parent, name) : NIL_VAL;
            if (IS_NIL(callee))
                return error_at(I, n, "%s has no method '%s' to call with super", parent ? parent->name->chars : "Object",
                                name->chars);
        } else {
            for (ObjClass *c = parent; c && IS_NIL(callee); c = c->parent) callee = c->constructor;
            if (IS_NIL(callee)) {
                if (n->as.super_call.args.count == 0) return NIL_VAL;
                return error_at(I, n, "the parent class has no constructor to pass these arguments to");
            }
        }
        Place place;
        place.root = n->as.super_call.self;
        place.nkeys = 0;
        if (place_changeable(I, &place))
            return invoke(I, n, retain(callee), 1, NIL_VAL, &place, &n->as.super_call.args, 0);
        return invoke(I, n, retain(callee), 1, retain(SLOT(0)), NULL, &n->as.super_call.args, 1);
    }

    case N_IS: {
        Value v = eval_value(I, n->as.is.value);
        if (IS_ERR(v)) return v;
        int r;
        if (n->as.is.tag >= 0) {
            r = value_has_type(v, n->as.is.tag);
        } else {
            Node *cn = n->as.is.cls;
            Value c = get_var(I, cn, &cn->as.var);
            if (IS_ERR(c)) {
                release(v);
                return c;
            }
            if (!IS_CLASS(c)) {
                Value e = error_at(I, n, "'%s' is not a type or a class", cn->as.var.name->chars);
                release(c);
                release(v);
                return e;
            }
            r = IS_INSTANCE(v) && is_subclass(AS_MAP(v)->klass, AS_CLASS(c));
            release(c);
        }
        release(v);
        return BOOL_VAL(r);
    }

    case N_ASSIGN:
        return eval_assign(I, n);

    case N_UNARY: {
        Value v = eval_value(I, n->as.unary.operand);
        if (IS_ERR(v)) return v;
        if (n->as.unary.op == T_MINUS) {
            if (IS_INT(v)) {
                I->frame->line = n->line;
                return int_negate(I, v);
            }
            if (!IS_NUM(v)) {
                Value e = error_at(I, n, "cannot negate %s", a_type(v));
                release(v);
                return e;
            }
            return NUM_VAL(-v.as.number);
        }
        int t = is_truthy(v);
        release(v);
        return BOOL_VAL(!t);
    }

    case N_BINARY: {
        Value a = eval_value(I, n->as.binary.left);
        if (IS_ERR(a)) return a;
        Value b = eval_value(I, n->as.binary.right);
        if (IS_ERR(b)) {
            release(a);
            return b;
        }
        Value r = binary_op(I, n, n->as.binary.op, a, b);
        release(a);
        release(b);
        return r;
    }

    case N_AND:
    case N_OR: {
        Value a = eval_value(I, n->as.binary.left);
        if (IS_ERR(a)) return a;
        int t = is_truthy(a);
        if (n->kind == N_AND ? !t : t) return a;
        release(a);
        return eval(I, n->as.binary.right);
    }

    case N_CALL: {
        Node *cn = n->as.call.callee;
        /* a class named here is called, not turned into a default instance */
        Value callee = cn->kind == N_VAR ? get_var(I, cn, &cn->as.var) : eval_value(I, cn);
        if (IS_ERR(callee)) return callee;
        if (!IS_CALLABLE(callee) && !IS_CLASS(callee)) {
            Value e = cn->kind == N_VAR
                          ? error_at(I, n, "'%s' is not a function (it is %s)", cn->as.var.name->chars,
                                     a_type(callee))
                          : error_at(I, n, "cannot call %s", a_type(callee));
            release(callee);
            return e;
        }
        return invoke(I, n, callee, 0, NIL_VAL, NULL, &n->as.call.args, 0);
    }

    case N_METHOD:
        return eval_method(I, n);

    case N_INDEX: {
        Value obj = eval_value(I, n->as.index.object);
        if (IS_ERR(obj)) return obj;
        Value idx = eval_value(I, n->as.index.index);
        if (IS_ERR(idx)) {
            release(obj);
            return idx;
        }
        Value r = index_get(I, n, obj, idx);
        release(obj);
        release(idx);
        return r;
    }

    case N_FIELD: {
        Value obj = eval_value(I, n->as.field.object);
        if (IS_ERR(obj)) return obj;
        Value r = field_get(I, n, obj, n->as.field.name);
        release(obj);
        return r;
    }

    case N_FUNC:
        return make_closure(I, n->as.func);

    case N_RANGE: {
        Value a = eval_value(I, n->as.range.start);
        if (IS_ERR(a)) return a;
        if (!IS_NUMERIC(a)) {
            Value e = error_at(I, n, "range start must be a Number, not %s", a_type(a));
            release(a);
            return e;
        }
        Value b = eval_value(I, n->as.range.end);
        if (IS_ERR(b)) return b;
        if (!IS_NUMERIC(b)) {
            Value e = error_at(I, n, "range end must be a Number, not %s", a_type(b));
            release(b);
            return e;
        }
        return OBJ_VAL(new_range(numeric_value(a), numeric_value(b), 1, n->as.range.inclusive));
    }

    default:  /* LCOV_EXCL_START: the parser puts only expressions here (and & only in arguments) */
        return error_at(I, n, "internal error: not an expression");
        /* LCOV_EXCL_STOP */
    }
}

/* ======================================================================
 * Statements
 * ==================================================================== */

static Exec exec_stmts(Interp *I, NodeList *stmts) {
    for (int i = 0; i < stmts->count; i++) {
        Exec r = exec(I, stmts->items[i]);
        if (r != EX_NORMAL) return r;
    }
    return EX_NORMAL;
}

static Exec exec_block(Interp *I, Node *b) {
    Exec r = exec_stmts(I, &b->as.block.stmts);
    if (b->as.block.nslots) clear_slots(I, b->as.block.first_slot, b->as.block.nslots);
    return r;
}

LUBE_NOINLINE static Exec exec_for(Interp *I, Node *n) {
    /* The loop owns a reference to what it iterates, so changes made to the
     * variable inside the loop go to a copy: iteration is never disturbed. */
    Value it = eval_value(I, n->as.for_s.iter);
    if (IS_ERR(it)) return EX_ERROR;
    int s1 = n->as.for_s.first.index, s2 = n->as.for_s.second.index, two = n->as.for_s.two;
    NodeList *body = &n->as.for_s.body->as.block.stmts;
    Exec result = EX_NORMAL;

#define RUN_BODY()                                                  \
    do {                                                            \
        Exec r_ = exec_stmts(I, body);                              \
        if (r_ == EX_BREAK) goto done;                              \
        if (r_ == EX_RETURN || r_ == EX_ERROR) {                    \
            result = r_;                                            \
            goto done;                                              \
        }                                                           \
    } while (0)

    if (IS_RANGE(it)) {
        ObjRange *r = AS_RANGE(it);
        double count = range_length(r);
        for (double k = 0; k < count; k++) {
            if (two) set_slot(I, s1, NUM_VAL(k));
            set_slot(I, two ? s2 : s1, NUM_VAL(r->start + k * r->step));
            RUN_BODY();
        }
    } else if (IS_LIST(it)) {
        ObjList *l = AS_LIST(it);
        for (size_t k = 0; k < l->count; k++) {
            if (two) set_slot(I, s1, NUM_VAL((double)k));
            set_slot(I, two ? s2 : s1, retain(l->items[k]));
            RUN_BODY();
        }
    } else if (IS_TEXT(it)) {
        ObjString *s = text_of(it);
        size_t i = 0;
        double k = 0;
        while (i < s->length) {  /* by UTF-8 character */
            size_t len = utf8_seq_len((unsigned char)s->chars[i]);
            if (i + len > s->length) len = s->length - i;
            Value ch = len == 1 ? retain(OBJ_VAL(I->chars[(unsigned char)s->chars[i]]))
                                : OBJ_VAL(new_string(s->chars + i, len));
            if (two) set_slot(I, s1, NUM_VAL(k));
            set_slot(I, two ? s2 : s1, ch);
            i += len;
            k++;
            RUN_BODY();
        }
    } else if (IS_MAP(it)) {
        ObjMap *m = AS_MAP(it);
        for (size_t i = 0; i < m->used; i++) {
            if (!m->entries[i].live) continue;
            set_slot(I, s1, retain(m->entries[i].key));
            if (two) set_slot(I, s2, retain(m->entries[i].value));
            RUN_BODY();
        }
    } else {
        error_at(I, n, "cannot loop over %s (expected a List, Range, str or Object)", a_type(it));
        result = EX_ERROR;
    }
#undef RUN_BODY

done:
    clear_slots(I, n->as.for_s.first_slot, n->as.for_s.nslots);
    release(it);
    return result;
}

LUBE_NOINLINE static Exec exec_try(Interp *I, Node *n) {
    Exec r = exec_block(I, n->as.try_s.body);
    if (r != EX_ERROR) return r;
    Value err = I->error_val;
    I->error_val = NIL_VAL;
    trace_reset(I);
    if (!n->as.try_s.has_var) {
        release(err);
        return exec_block(I, n->as.try_s.handler);
    }
    set_slot(I, n->as.try_s.var.index, err);
    r = exec_stmts(I, &n->as.try_s.handler->as.block.stmts);
    clear_slots(I, n->as.try_s.first_slot, n->as.try_s.nslots);
    return r;
}

static void keep_arena(Interp *I, Arena *a) {
    if (I->narenas == I->cap_arenas) {
        I->cap_arenas = I->cap_arenas ? I->cap_arenas * 2 : 8;
        I->arenas = xrealloc(I->arenas, sizeof(Arena *) * (size_t)I->cap_arenas);
    }
    I->arenas[I->narenas++] = a;
}

/* Runs top-level statements in a new frame. */
static Exec run_top(Interp *I, Node *program, Source *src, const char *name, int repl) {
    int nslots = program->as.block.frame_slots;
    if (!stack_room(I, nslots)) {  /* LCOV_EXCL_START: the C stack limit is always reached first */
        lube_error(I, "stack overflow: too much recursion");
        return EX_ERROR;
    }  /* LCOV_EXCL_STOP */
    int base = I->sp;
    for (int i = 0; i < nslots; i++) I->stack[base + i] = NIL_VAL;
    I->sp = base + nslots;

    Frame frame;
    frame.prev = I->frame;
    frame.name = name;
    frame.src = src;
    frame.line = 1;
    frame.base = base;
    frame.fn = NULL;
    I->frame = &frame;

    Exec r = EX_NORMAL;
    NodeList *stmts = &program->as.block.stmts;
    for (int i = 0; i < stmts->count && r != EX_ERROR; i++) {
        Node *s = stmts->items[i];
        if (repl && i == stmts->count - 1 && s->kind == N_EXPR && s->as.expr->kind != N_ASSIGN) {
            frame.line = s->line;
            Value v = eval(I, s->as.expr);
            if (IS_ERR(v)) {
                r = EX_ERROR;
            } else if (!IS_NIL(v)) {
                ObjString *text = value_repr(v);
                printf("%s\n", text->chars);
                release(OBJ_VAL(text));
            }
            release(v);
        } else {
            r = exec(I, s);
        }
    }
    if (r == EX_ERROR) trace_frame(I, &frame);
    pop_to(I, base);
    I->frame = frame.prev;
    return r;
}

LUBE_NOINLINE /* class Dog extends Animal { ... }: builds the class (once, at the top level). */
LUBE_NOINLINE static Exec exec_class(Interp *I, Node *n) {
    ObjClass *parent = NULL;
    if (n->as.klass.parent) {
        Node *pn = n->as.klass.parent;
        Value p = get_var(I, pn, &pn->as.var);
        if (IS_ERR(p)) return EX_ERROR;
        parent = IS_CLASS(p) ? AS_CLASS(p) : NULL;  /* classes live until shutdown */
        release(p);
        if (!parent) {
            error_at(I, n, "'%s' is not a class, so %s cannot extend it", pn->as.var.name->chars,
                     n->as.klass.var.name->chars);
            return EX_ERROR;
        }
    }
    NodeList *fields = &n->as.klass.fields;
    int inherited = parent ? parent->nfields : 0, total = inherited + fields->count;
    ObjClass *c = (ObjClass *)alloc_object(sizeof(ObjClass), OBJ_CLASS);
    c->name = n->as.klass.var.name;
    c->parent = parent;
    c->nfields = 0;
    c->field_names = xmalloc(sizeof(ObjString *) * (size_t)(total ? total : 1));
    c->field_defaults = xmalloc(sizeof(Value) * (size_t)(total ? total : 1));
    c->field_types = xmalloc(sizeof(TypeDesc) * (size_t)(total ? total : 1));
    c->methods = new_map();
    c->constructor = NIL_VAL;
    list_push(I->classes, OBJ_VAL(c));  /* keeps it alive until shutdown */

    for (int i = 0; i < inherited; i++, c->nfields++) {  /* the parent's fields come first */
        c->field_names[i] = parent->field_names[i];
        c->field_defaults[i] = retain(parent->field_defaults[i]);
        c->field_types[i] = parent->field_types[i];
    }
    for (int i = 0; i < fields->count; i++) {
        Node *f = fields->items[i];
        for (int j = 0; j < inherited; j++) {
            if (c->field_names[j] == f->as.let.var.name) {
                error_at(I, f, "field '%s' is already declared in %s", f->as.let.var.name->chars,
                         parent->name->chars);
                return EX_ERROR;
            }
        }
        I->frame->line = f->line;
        Value v = eval_value(I, f->as.let.value);  /* each instance starts with a copy of this */
        if (IS_ERR(v)) return EX_ERROR;
        c->field_names[c->nfields] = f->as.let.var.name;
        c->field_defaults[c->nfields] = v;
        c->field_types[c->nfields] = desc_of(v);
        c->nfields++;
    }
    for (int i = 0; i < n->as.klass.methods.count; i++) {
        FuncDecl *d = n->as.klass.methods.items[i]->as.func;
        d->owner = c;
        map_set(c->methods, retain(OBJ_VAL(d->name)), make_closure(I, d));
    }
    if (n->as.klass.ctor) {
        n->as.klass.ctor->as.func->owner = c;
        c->constructor = make_closure(I, n->as.klass.ctor->as.func);
    }
    ObjString *name = n->as.klass.var.name;
    map_set(I->globals, retain(OBJ_VAL(name)), retain(OBJ_VAL(c)));
    map_set(I->constants, retain(OBJ_VAL(name)), NUM_VAL(ORIGIN_CLASS));
    return EX_NORMAL;
}

static Exec exec_import(Interp *I, Node *n) {
    ObjString *rel = n->as.import.path;
    const char *current = I->frame->src ? I->frame->src->path : "";

    StrBuf path;
    sb_init(&path);
    if (rel->chars[0] != '/') {  /* relative to the importing file */
        const char *slash = strrchr(current, '/');
        if (slash) sb_putn(&path, current, (size_t)(slash - current + 1));
    }
    sb_putn(&path, rel->chars, rel->length);
    const char *last = strrchr(path.data, '/');
    if (!strchr(last ? last : path.data, '.')) sb_puts(&path, ".lube");

    Value key = OBJ_VAL(new_string(path.data, path.len));
    if (map_find(I->modules, key)) {  /* each file runs only once */
        release(key);
        sb_free(&path);
        return EX_NORMAL;
    }
    size_t len;
    char *text = read_file(path.data, &len);
    if (!text) {
        release(key);
        sb_free(&path);
        error_at(I, n, "cannot import '%s': file not found", rel->chars);
        return EX_ERROR;
    }
    map_set(I->modules, key, BOOL_VAL(1));
    Source *src = lube_add_source(I, path.data, text, len);
    sb_free(&path);

    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (program && !resolve_program(I, program, src, arena)) {
        arena_free(arena);
        program = NULL;
    }
    if (!program) {
        error_at(I, n, "cannot import '%s': the file has errors", rel->chars);
        return EX_ERROR;
    }
    keep_arena(I, arena);
    return run_top(I, program, src, "<module>", 0) == EX_ERROR ? EX_ERROR : EX_NORMAL;
}

static Exec exec(Interp *I, Node *n) {
    I->frame->line = n->line;
    switch (n->kind) {
    case N_EXPR: {
        Value v = eval(I, n->as.expr);
        if (IS_ERR(v)) return EX_ERROR;
        release(v);
        return EX_NORMAL;
    }

    case N_LET: {
        Value v = n->as.let.value ? eval_value(I, n->as.let.value) : NIL_VAL;
        if (IS_ERR(v)) return EX_ERROR;
        VarRef *r = &n->as.let.var;
        if (r->kind == VAR_LOCAL) {
            I->decl[I->frame->base + r->index] = desc_of(v);  /* the var keeps this type */
            set_slot(I, r->index, v);
            return EX_NORMAL;
        }
        Value *g = map_find(I->globals, OBJ_VAL(r->name));
        if (g && g->type == VAL_LENT) {
            release(v);
            error_at(I, n, IN_USE, r->name->chars);
            return EX_ERROR;
        }
        set_global_type(I, r->name, desc_of(v));
        map_set(I->globals, retain(OBJ_VAL(r->name)), v);
        if (n->as.let.is_var)
            map_delete(I->constants, OBJ_VAL(r->name), NULL);
        else
            map_set(I->constants, retain(OBJ_VAL(r->name)), NUM_VAL(n->as.let.is_fn ? ORIGIN_FN : ORIGIN_LET));
        return EX_NORMAL;
    }

    case N_BLOCK:
        return exec_block(I, n);

    case N_IF: {
        Value c = eval_value(I, n->as.if_s.cond);
        if (IS_ERR(c)) return EX_ERROR;
        int t = is_truthy(c);
        release(c);
        if (t) return exec_block(I, n->as.if_s.then_branch);
        if (n->as.if_s.else_branch) return exec(I, n->as.if_s.else_branch);
        return EX_NORMAL;
    }

    case N_WHILE:
        for (;;) {
            Value c = eval_value(I, n->as.while_s.cond);
            if (IS_ERR(c)) return EX_ERROR;
            int t = is_truthy(c);
            release(c);
            if (!t) break;
            Exec r = exec_block(I, n->as.while_s.body);
            if (r == EX_BREAK) break;
            if (r == EX_RETURN || r == EX_ERROR) return r;
        }
        return EX_NORMAL;

    case N_FOR:
        return exec_for(I, n);

    case N_RETURN: {
        Value v = n->as.expr ? eval(I, n->as.expr) : NIL_VAL;
        if (IS_ERR(v)) return EX_ERROR;
        release(I->ret_val);
        I->ret_val = v;
        return EX_RETURN;
    }

    case N_BREAK:
        return EX_BREAK;

    case N_CONTINUE:
        return EX_CONTINUE;

    case N_THROW: {
        Value v = eval_value(I, n->as.expr);
        if (IS_ERR(v)) return EX_ERROR;
        I->frame->line = n->line;
        lube_throw(I, v);
        return EX_ERROR;
    }

    case N_TRY:
        return exec_try(I, n);

    case N_IMPORT:
        return exec_import(I, n);

    case N_CLASS:
        return exec_class(I, n);

    default:  /* LCOV_EXCL_START: expression nodes are always wrapped in N_EXPR */
        return EX_NORMAL;
        /* LCOV_EXCL_STOP */
    }
}

/* ======================================================================
 * Running programs
 * ==================================================================== */

LUBE_COLD static void report_uncaught(Interp *I) {
    fflush(stdout);
    Value e = I->error_val;
    I->error_val = NIL_VAL;
    ObjString *msg = IS_TEXT(e) ? (ObjString *)retain(OBJ_VAL(text_of(e))).as.obj : value_repr(e);
    Source *src = I->error_src;
    fprintf(stderr, "%s:%d: error: %s\n", src ? src->path : "<input>", I->error_line, msg->chars);
    print_source_line(src, I->error_line, 0);
    trace_flush_repeats(I);
    if (I->trace_count > 1) {
        fprintf(stderr, "traceback (innermost call first):\n%s", I->trace.data ? I->trace.data : "");
        if (I->trace_hidden > 0) {
            if (I->trace_hidden > 1) fprintf(stderr, "  ... %d more calls ...\n", I->trace_hidden - 1);
            fputs(I->trace_last, stderr);
        }
    }
    release(OBJ_VAL(msg));
    release(e);
    trace_reset(I);
}

/* Parses and runs a source. In REPL mode the value of a final expression is
 * printed. Returns a process exit status. */
int lube_run_source(Interp *I, Source *src, int repl) {
    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (!program) return 65;
    if (!resolve_program(I, program, src, arena)) {
        arena_free(arena);
        return 65;
    }
    keep_arena(I, arena);  /* functions defined here may outlive this run */
    if (run_top(I, program, src, "<script>", repl) == EX_ERROR) {
        report_uncaught(I);
        return 70;
    }
    return 0;
}

/* Checks a program without running any of it: syntax and resolver errors
 * (the same ones that stop a run with status 65). Returns 0 or 65. */
int lube_check_source(Interp *I, Source *src) {
    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (!program) return 65;
    int ok = resolve_program(I, program, src, arena);
    arena_free(arena);
    return ok ? 0 : 65;
}

Source *lube_add_source(Interp *I, const char *path, char *text, size_t length) {
    Source *s = xmalloc(sizeof(Source));
    s->path = xstrndup(path, strlen(path));
    s->text = text;
    s->length = length;
    s->strict = 0;
    s->next = I->sources;
    I->sources = s;
    return s;
}

int lube_run_file(Interp *I, const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        fprintf(stderr, "lube: cannot open file '%s'\n", path);
        return 66;
    }
    return lube_run_source(I, lube_add_source(I, path, text, len), 0);
}

/* Defines a built-in global (cannot be assigned to). Takes over v. */
void lube_define(Interp *I, const char *name, Value v) {
    ObjString *key = lube_intern(I, name, strlen(name));
    map_set(I->globals, retain(OBJ_VAL(key)), v);
    map_set(I->constants, retain(OBJ_VAL(key)), NUM_VAL(ORIGIN_BUILTIN));
}

Interp *lube_new(char *stack_base) {
    Interp *I = calloc(1, sizeof(Interp));
    if (!I) lube_out_of_memory(); /* LCOV_EXCL_LINE: only if the system has no memory left */
    I->ret_val = NIL_VAL;
    I->error_val = NIL_VAL;
    I->stack_base = stack_base;
    size_t margin = LUBE_STACK_SIZE / 8 > 512u * 1024u ? LUBE_STACK_SIZE / 8 : 512u * 1024u;
    I->stack_budget = LUBE_STACK_SIZE - margin;
    I->rng = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)I << 16) ^ 0x9E3779B97F4A7C15ULL;
    if (I->rng == 0) I->rng = 1;
    sb_init(&I->trace);

    I->decl = calloc(LUBE_STACK_MAX, sizeof(TypeDesc));
    if (!I->decl) lube_out_of_memory();
    I->strings = new_map();
    I->globals = new_map();
    I->constants = new_map();
    I->modules = new_map();
    I->global_type_index = new_map();
    I->classes = new_list();
    for (int c = 0; c < 256; c++) {
        char ch = (char)c;
        I->chars[c] = new_string(&ch, 1);
    }
    register_builtins(I);
    return I;
}

/* Frees everything. Returns the number of objects still alive afterwards,
 * which is always 0 unless there is a bug. */
size_t lube_free(Interp *I) {
    pop_to(I, 0);
    release(I->ret_val);
    release(I->error_val);
    release(OBJ_VAL(I->globals));
    release(OBJ_VAL(I->constants));
    release(OBJ_VAL(I->modules));
    release(OBJ_VAL(I->global_type_index));
    release(OBJ_VAL(I->classes));  /* after every instance is gone */
    free(I->global_types);
    free(I->decl);
    for (int c = 0; c < 256; c++) release(OBJ_VAL(I->chars[c]));
    for (int i = 0; i < I->narenas; i++) arena_free(I->arenas[i]);
    free(I->arenas);
    release(OBJ_VAL(I->strings));  /* last: the syntax trees borrowed these */
    Source *s = I->sources;
    while (s) {
        Source *next = s->next;
        free(s->path);
        free(s->text);
        free(s);
        s = next;
    }
    sb_free(&I->trace);
    free(I);
    return lube_live_objects;
}
