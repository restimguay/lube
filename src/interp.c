/*
 * interp.c - the virtual machine that runs Lube's bytecode (see compile.c),
 * and the runtime it shares with the standard library.
 *
 * Ownership rules (there is no garbage collector):
 *   - Every value on the stack is owned by it; an instruction takes over the
 *     values it pops and pushes new references.
 *   - Errors are ordinary return values (ERR_VAL, with the error in
 *     I->error_val) that every caller checks, so each function releases what
 *     it holds on every path. Nothing jumps over cleanup code.
 *   - A call to a Lube function runs in the same VM loop: it gets a frame
 *     (parameters and locals in I->stack, then its operand stack) and does
 *     not use the C stack, so deep recursion needs no C stack at all.
 *     Leaving a block clears its slots, so values are freed as they go out
 *     of scope.
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


#ifndef LUBE_MAX_FRAMES
#define LUBE_MAX_FRAMES 100000      /* calls deep (each frame is allocated as needed) */
#endif

static Value bad_key(Interp *I, ObjMap *m, Value key);
static Value call_value(Interp *I, Value callee, int argc, int base);
static Value package_member(Interp *I, Value pkgv, ObjString *name);

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

/* The line a frame is at: the running instruction's, for bytecode. */
static int frame_line(Frame *f) {
#if VM_SLIM_CALL
    return f->proto->code.lines[f->pc - f->proto->code.code];
#else
    return f->proto && f->pc ? f->proto->code.lines[f->pc - f->proto->code.code] : f->line;
#endif
}

int lube_frame_line(Frame *f) { return frame_line(f); }

static const char *callee_name(Value f);

/* A frame's name, as tracebacks and errors give it: its function's (a constructor's is its class's), or the
 * script's or the `on` block's. (A slim frame does not hold it: its code says.) */
const char *lube_frame_name(Frame *f) {
#if VM_SLIM_CALL
    return f->block || !f->fn ? f->proto->name->chars : callee_name(OBJ_VAL(f->fn));
#else
    return f->name;
#endif
}

Source *lube_frame_src(Frame *f) {
#if VM_SLIM_CALL
    return f->proto->src;
#else
    return f->src;
#endif
}

#ifdef VM_PC_CHECK  /* a checking build: is the VM's state the Interp's whenever something can see it? */
#include <stdio.h>
#include <stdlib.h>
static LUBE_THREAD_LOCAL int vm_synced = 1;  /* 0 while execute()'s loop has the state in its own registers */
static void vm_check_pc(Interp *I) {
    (void)I;
    if (!vm_synced) {
        fprintf(stderr, "VM_PC_CHECK: an error is raised while the loop's registers are not written back\n");
        abort();
    }
}
#endif

/* Raises v (taking over the reference). Always returns ERR_VAL. */
Value lube_throw(Interp *I, Value v) {
#ifdef VM_PC_CHECK
    vm_check_pc(I);
#endif
    release(I->error_val);
    I->error_val = v;
    I->error_line = I->frame ? frame_line(I->frame) : 0;
    I->error_src = I->frame ? lube_frame_src(I->frame) : NULL;
    trace_reset(I);
    if (I->dbg) debug_error(I);  /* break on errors: the frames are all still here */
    return ERR_VAL;
}

/* A message made of text, %s, %d and %lu alone (most are), written without printf, which costs more than
 * the rest of raising an error that a program catches: its length, cut short as vsnprintf's would be, or -1
 * for a format with anything else in it (nothing is taken from ap then). */
static int plain_message(char *buf, size_t size, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == 'l' && p[1] == 'u') p++;
        else if (*p != 's' && *p != 'd') return -1;
    }
    size_t n = 0;
    for (const char *p = fmt; *p;) {
        char digits[24];
        const char *s = p;
        size_t len = 0;
        if (*p != '%') {  /* the text up to the next % */
            while (p[len] && p[len] != '%') len++;
            p += len;
        } else if (p[1] == 's') {
            s = va_arg(ap, const char *);
            len = strlen(s);
            p += 2;
        } else {  /* a whole number: its digits, from the last */
            int is_int = p[1] == 'd';
            long d = is_int ? va_arg(ap, int) : 0;
            unsigned long u = is_int ? (d < 0 ? 0 - (unsigned long)d : (unsigned long)d) : va_arg(ap, unsigned long);
            char *q = digits + sizeof digits;
            do *--q = (char)('0' + u % 10);
            while (u /= 10);
            q[-1] = '-';
            q -= d < 0;
            s = q;
            len = (size_t)(digits + sizeof digits - q);
            p += is_int ? 2 : 3;
        }
        if (len > size - 1 - n) len = size - 1 - n;
        memcpy(buf + n, s, len);
        n += len;
    }
    buf[n] = 0;
    return (int)n;
}

LUBE_COLD static Value verror(Interp *I, const char *fmt, va_list ap) {
    char buf[1024];
    int n = plain_message(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        vsnprintf(buf, sizeof buf, fmt, ap);
        n = (int)strlen(buf);
    }
    return lube_throw(I, OBJ_VAL(new_string(buf, (size_t)n)));
}

Value lube_error(Interp *I, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Value r = verror(I, fmt, ap);
    va_end(ap);
    return r;
}

LUBE_COLD static Value cannot_change(Interp *I, const char *name, Origin origin) {
    char msg[400];
    unchangeable_message(msg, sizeof msg, name, origin);
    return lube_error(I, "%s", msg);
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
    Source *src = lube_frame_src(f);
    snprintf(line, sizeof line, "  at %s (%s:%d)\n", lube_frame_name(f), src ? src->path : "?", frame_line(f));
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
static Value keep_type(Interp *I, const VarRef *r, Value v) {
    TypeDesc *d = r->kind == VAR_LOCAL ? &I->decl[I->frame->base + r->index] : global_type(I, r->name);
    Value out;
    if (!d || desc_accepts(*d, v, &out)) return d ? out : v;
    const char *hint = d->kind == TD_STROBJ && IS_STRING(v) ? " (convert it with String(x))"
                       : d->kind == TD_STR && IS_STROBJ(v) ? " (use x.value for its str)" : "";
    Value e = lube_error(I, "'%s' is %s, so it cannot hold %s: a var keeps the type it starts with%s",
                       lube_shown(r->name), with_article(desc_name(*d)), a_type(v), hint);
    release(v);
    return e;
}

/* A class field keeps the type it starts with. Takes over v. */
static Value keep_field_type(Interp *I, const TypeDesc *d, Value field, Value holder, Value v) {
    Value out;
    if (desc_accepts(*d, v, &out)) return out;
    Value e = lube_error(I, "field '%s' of %s is %s, so it cannot hold %s", AS_STRING(field)->chars,
                         type_name(holder), with_article(desc_name(*d)), a_type(v));
    release(v);
    return e;
}

static int keys_same(Value a, Value b) {
    ObjString *x = AS_STRING(a), *y = AS_STRING(b);
    return x == y || (x->length == y->length && memcmp(x->chars, y->chars, x->length) == 0);
}

/* Where field `name` (an interned name) is in instance m: an instance has
 * its class's fields in their order, and none is ever removed, so field f
 * is entry f. -1 if the name is not found this way (the caller then looks
 * the slow way, which also explains what is wrong). */
static inline int field_slot(ObjMap *m, ObjString *name) {
    ObjClass *c = m->klass;
    for (int f = 0; f < c->nfields; f++)
        if (c->field_names[f] == name)
            return (size_t)f < m->used && m->entries[f].live && m->entries[f].key.as.obj == (Obj *)name ? f : -1;
    return -1;
}

static Value find_method(ObjClass *c, ObjString *name) {
    Value m;
    for (; c; c = c->parent)
        if (map_get(c->methods, OBJ_VAL(name), &m)) return m;
    return NIL_VAL;
}

Value lube_find_method(ObjClass *c, ObjString *name) { return find_method(c, name); }

/* Makes a global (let, var or fn: kind 1, 0 or 2). Takes over v. */
static int define_global(Interp *I, ObjString *name, Value v, int kind) {
    Value *g = map_find(I->globals, OBJ_VAL(name));
    if (g && g->type == VAL_LENT) {
        release(v);
        lube_error(I, IN_USE, lube_shown(name));
        return -1;
    }
    set_global_type(I, name, desc_of(v));
    map_set(I->globals, retain(OBJ_VAL(name)), v);
    if (kind == 0) map_delete(I->constants, OBJ_VAL(name), NULL);
    else map_set(I->constants, retain(OBJ_VAL(name)), NUM_VAL(kind == 2 ? ORIGIN_FN : ORIGIN_LET));
    I->epoch++;
    return 0;
}

/* A package's name that never changes, as a fn's (plugin.c's functions and types). Takes v. */
int lube_define_global(Interp *I, ObjString *key, Value v) { return define_global(I, key, v, 2); }
int lube_assign_global(Interp *I, ObjString *key, Value v) { return define_global(I, key, v, 0); }

/* In an isolate, a global variable gets its value the first time it is
 * used. 1 = defined now, 0 = no such global waiting, -1 = its starting value
 * raised an error. */
static int init_lazy(Interp *I, ObjString *name) {
    Value entry;
    if (!I->lazy || !map_get(I->lazy, OBJ_VAL(name), &entry)) return 0;
    entry = retain(entry);
    map_delete(I->lazy, OBJ_VAL(name), NULL);  /* gone while it runs: a value that needs itself is undefined */
    ObjList *e = AS_LIST(entry);
    Value v = lube_call(I, e->items[0], 0, NULL);
    int kind = (int)e->items[1].as.number;
    release(entry);
    if (!IS_ERR(v) && IS_NIL(v))
        v = lube_error(I, "this function call does not return a value, so there is nothing to use here");
    if (IS_ERR(v)) return -1;
    define_global(I, name, v, kind);
    return 1;
}

/* A global's slot (after its first use in an isolate), or NULL: *failed
 * says whether that was an error raised while making it. */
static Value *lookup_global(Interp *I, ObjString *name, int *failed) {
    Value *g = map_find(I->globals, OBJ_VAL(name));
    *failed = 0;
    if (g || !I->lazy) return g;
    int r = init_lazy(I, name);
    *failed = r < 0;
    return r > 0 ? map_find(I->globals, OBJ_VAL(name)) : NULL;
}

/* The global r names, found through r's cache (checked: the entry must
 * still be that global's, as in global_slot). */
static Value *global_ref(Interp *I, const VarRef *r, int *failed) {
    ObjMap *g = I->globals;
    int at = r->gslot - 1;
    if (at >= 0 && (size_t)at < g->used && g->entries[at].live && g->entries[at].key.as.obj == (Obj *)r->name) {
        *failed = 0;
        return &g->entries[at].value;
    }
    Value *v = lookup_global(I, r->name, failed);
    if (v) ((VarRef *)r)->gslot = (int)((MapEntry *)(void *)((char *)v - offsetof(MapEntry, value)) - I->globals->entries) + 1;
    return v;
}

/* Is there a global r names (through r's cache)? (Not made, when lazy.) */
static int global_exists(Interp *I, const VarRef *r) {
    ObjMap *g = I->globals;
    int at = r->gslot - 1;
    if (at >= 0 && (size_t)at < g->used && g->entries[at].live && g->entries[at].key.as.obj == (Obj *)r->name) return 1;
    return map_find(g, OBJ_VAL(r->name)) != NULL;
}

/* global_origin for r, through r's cache. */
static int global_origin_ref(Interp *I, const VarRef *r, Origin *origin) {
    if (r->cepoch != I->epoch) {
        Origin o;
        ((VarRef *)r)->corigin = global_origin(I, r->name, &o) ? (int)o : -1;
        ((VarRef *)r)->cepoch = I->epoch;
    }
    if (r->corigin < 0) return 0;
    *origin = (Origin)r->corigin;
    return 1;
}

static Value get_var(Interp *I, const VarRef *r) {
    switch (r->kind) {
    case VAR_LOCAL: return retain(SLOT(r->index));
    case VAR_CAPTURE: return retain(I->frame->fn->captures[r->index]);
    case VAR_SELF: return retain(OBJ_VAL(I->frame->fn));
    case VAR_GLOBAL: break;
    }
    int failed;
    Value *g = global_ref(I, r, &failed);
    if (failed) return ERR_VAL;
    if (!g) return lube_error(I, "undefined variable '%s'", lube_shown(r->name));
    if (g->type == VAL_LENT) return lube_error(I, IN_USE, lube_shown(r->name));
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
    case T_AT: return "@";
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

static Value repeat(Interp *I, Value seq, double times) {
    if (!(times >= 0) || times != floor(times)) return lube_error(I, "can only repeat by a whole, non-negative number");
    size_t unit = IS_TEXT(seq) ? text_of(seq)->length : AS_LIST(seq)->count;
    size_t limit = IS_TEXT(seq) ? LUBE_MAX_BYTES : LUBE_MAX_ITEMS;
    if (unit == 0) times = 0;
    if (times > (double)(limit / (unit ? unit : 1))) return lube_error(I, "the repeated result would be too large");
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
/* a op b for two plain Numbers, without a call: 1 with *out set; 0 leaves
 * it to binary_op (other operators, division by zero, % of other numbers). */
static inline int num_op(int op, double x, double y, Value *out) {
    switch (op) {
    case T_PLUS: *out = NUM_VAL(x + y); return 1;
    case T_MINUS: *out = NUM_VAL(x - y); return 1;
    case T_STAR: *out = NUM_VAL(x * y); return 1;
    case T_SLASH:
        if (y == 0) return 0;  /* the error comes from binary_op */
        *out = NUM_VAL(x / y);
        return 1;
    case T_PERCENT:  /* positive whole numbers */
        if (!(x > 0 && y > 0 && x < 9e15 && y < 9e15 && x == (double)(int64_t)x && y == (double)(int64_t)y)) return 0;
        *out = NUM_VAL((double)((int64_t)x % (int64_t)y));
        return 1;
    case T_LT: *out = BOOL_VAL(x < y); return 1;
    case T_LE: *out = BOOL_VAL(x <= y); return 1;
    case T_GT: *out = BOOL_VAL(x > y); return 1;
    case T_GE: *out = BOOL_VAL(x >= y); return 1;
    case T_EQ: *out = BOOL_VAL(x == y); return 1;
    case T_NEQ: *out = BOOL_VAL(x != y); return 1;
    default: return 0;
    }
}

/* a == b or a != b for two strs, without the general comparison: 1 and *out, or 0 for anything else */
static inline int str_eq_op(int op, Value a, Value b, Value *out) {
    if ((op != T_EQ && op != T_NEQ) || !IS_STRING(a) || !IS_STRING(b)) return 0;
    ObjString *x = AS_STRING(a), *y = AS_STRING(b);
    int eq = x == y || (x->length == y->length && memcmp(x->chars, y->chars, x->length) == 0);
    *out = BOOL_VAL(eq == (op == T_EQ));
    return 1;
}

static Value binary_op(Interp *I, TokenType op, Value a, Value b) {
    Value quick;
    if (IS_NUM(a) && IS_NUM(b) && num_op(op, a.as.number, b.as.number, &quick)) return quick;
    if (IS_NUM(a) && IS_NUM(b)) {  /* what num_op leaves */
        double x = a.as.number, y = b.as.number;
        switch (op) {
        case T_SLASH: return lube_error(I, "division by zero");
        case T_PERCENT: {
            if (y == 0) return lube_error(I, "modulo by zero");
            if (fabs(x) < 9e15 && fabs(y) < 9e15 && x == floor(x) && y == floor(y)) {  /* whole numbers: no fmod */
                int64_t q = (int64_t)x % (int64_t)y;
                if (q == 0) return NUM_VAL(copysign(0.0, x));
                if ((q < 0) != (y < 0)) q += (int64_t)y;  /* the result takes the sign of y */
                return NUM_VAL((double)q);
            }
            double r = fmod(x, y);
            if (r != 0 && ((r < 0) != (y < 0))) r += y;  /* result takes the sign of y */
            return NUM_VAL(r);
        }
        case T_POW: return NUM_VAL(pow(x, y));
        default: break;  /* comparisons: below (the VM compares two plain Numbers itself) */
        }
    }
    if (IS_NUMERIC(a) && IS_NUMERIC(b)) {
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
    if ((IS_FOREIGN(a) || IS_FOREIGN(b)) && lube_foreign_binary(I, op, a, b, &quick)) return quick;  /* a plugin's */
    if ((IS_MONEY(a) || IS_MONEY(b)) && op != T_IN && op != T_AT && lube_money_op(I, op, a, b, &quick)) return quick;
    switch (op) {
    case T_EQ: return BOOL_VAL(values_equal(a, b));
    case T_NEQ: return BOOL_VAL(!values_equal(a, b));
    case T_IN:
        return in_op(I, a, b);
    case T_PLUS:
        if (IS_TEXT(a) || IS_TEXT(b)) return concat(a, b);
        if (IS_BYTES(a) && IS_BYTES(b)) return lube_bytes_concat(a, b);
        if (IS_LIST(a) && IS_LIST(b)) {  /* keeps the left list's element type */
            ObjList *x = AS_LIST(a), *y = AS_LIST(b);
            ObjList *out = new_list();
            out->elem = x->elem;
            list_reserve(out, x->count + y->count);
            for (size_t i = 0; i < x->count; i++) out->items[out->count++] = retain(x->items[i]);
            for (size_t i = 0; i < y->count; i++) {
                Value v = CHECK_ELEM(I, type_name(a), out->elem, y->items[i]);
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
                                             : bad_key(I, out, k);
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
        if ((IS_TEXT(a) || IS_LIST(a)) && IS_NUMERIC(b)) return repeat(I, a, numeric_value(b));
        if (IS_NUMERIC(a) && (IS_TEXT(b) || IS_LIST(b))) return repeat(I, b, numeric_value(a));
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
    return lube_error(I, "unsupported operand types for '%s': %s and %s", op_symbol(op), type_name(a), type_name(b));
}

/* A value entering a typed list or Map[K, V] (`holder` names it, `tag` is
 * the type it holds): returns a new reference to it (a plain whole number
 * becomes the integer type), or raises an error. */
Value lube_check_elem(Interp *I, const char *holder, int tag, Value v) {
    if (value_has_type(v, tag)) return retain(v);
    if (TT_IS_INT(tag) && IS_NUM(v)) return int_from_whole(I, v.as.number, tag - TT_INT8);
    return lube_error(I, "this %s can only hold %s values, not %s", holder, tag_name(tag), a_type(v));
}

/* The error for a key an Object or Map does not take. */
static Value bad_key(Interp *I, ObjMap *m, Value key) {
    if (m->dict)
        return lube_error(I, "this %s has %s keys, not %s", type_name(OBJ_VAL(m)), tag_name(m->ktype), a_type(key));
    return lube_error(I, "object keys must be str, Number or Bool values, not %s", a_type(key));
}

/* Arithmetic for C code (the standard library): same rules as the operators. */
Value lube_arith(Interp *I, TokenType op, Value a, Value b) { return binary_op(I, op, a, b); }

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
    if (IS_BYTES(c)) {  /* a byte, or a run of bytes (Bytes, or a str's) */
        ObjBytes *b = AS_BYTES(c);
        if (IS_BYTES(x)) return str_find((const char *)b->data, b->len, (const char *)AS_BYTES(x)->data, AS_BYTES(x)->len, 0) >= 0;
        if (IS_TEXT(x)) return str_find((const char *)b->data, b->len, text_of(x)->chars, text_of(x)->length, 0) >= 0;
        double d = IS_NUMERIC(x) ? numeric_value(x) : -1;
        return d >= 0 && d <= 255 && d == floor(d) && b->len && memchr(b->data, (int)d, b->len) != NULL;
    }
    lube_error(I, "cannot look inside %s (expected a List, str, Object, Range or Bytes)", a_type(c));
    return -1;
}

/* ======================================================================
 * Indexing and fields
 * ==================================================================== */

/* Checks an index (negative counts from the end). 1 = ok, 0 = error raised. */
static int list_index(Interp *I, Value idx, size_t len, const char *what, size_t *out) {
    if (!IS_NUMERIC(idx)) {
        lube_error(I, "%s index must be a Number, not %s", what, a_type(idx));
        return 0;
    }
    double d = numeric_value(idx);
    if (d != floor(d)) {
        lube_error(I, "%s index must be a whole number, got %g", what, d);
        return 0;
    }
    double i = d < 0 ? d + (double)len : d;
    if (i < 0 || i >= (double)len) {
        char buf[64];
        format_number(buf, sizeof buf, d);
        lube_error(I, "%s index %s is out of range (length %lu)", what, buf, (unsigned long)len);
        return 0;
    }
    *out = (size_t)i;
    return 1;
}

/* The error for x[i, j] on what takes one index. */
static Value one_index(Interp *I, Value obj, Value idx) {
    return lube_error(I, "%s takes one index, not %lu", a_type(obj), (unsigned long)AS_LIST(idx)->count);
}

static Value index_get(Interp *I, Value obj, Value idx) {
    size_t i;
    Value r;
    if (IS_FOREIGN(obj) && lube_foreign_index(I, obj, idx, &r)) return r;
    if (IS_TUPLE(idx)) return IS_FOREIGN(obj) ? lube_error(I, "cannot index into %s", a_type(obj)) : one_index(I, obj, idx);
    if (IS_LIST(obj)) {
        ObjList *l = AS_LIST(obj);
        return list_index(I, idx, l->count, "list", &i) ? retain(l->items[i]) : ERR_VAL;
    }
    if (IS_MAP(obj)) {
        ObjMap *m = AS_MAP(obj);
        Value v;
        if (m->dict && !map_key_ok(m, idx)) return bad_key(I, m, idx);
        if (map_key_ok(m, idx) && map_get(m, idx, &v)) return retain(v);
        ObjString *k = value_repr(idx);
        Value e = lube_error(I, "the %s has no key %s (check with has() first, or use get(%s, key, default))",
                           m->dict ? "map" : "object", k->chars, m->dict ? "map" : "object");
        release(OBJ_VAL(k));
        return e;
    }
    if (IS_TEXT(obj)) {
        ObjString *s = text_of(obj);
        if (!list_index(I, idx, s->length, "string", &i)) return ERR_VAL;
        return retain(OBJ_VAL(I->chars[(unsigned char)s->chars[i]]));
    }
    if (IS_RANGE(obj)) {  /* in floating point: a range can be longer than any list */
        ObjRange *r = AS_RANGE(obj);
        double count = range_length(r);
        double d = IS_NUMERIC(idx) ? numeric_value(idx) : 0.5;
        if (d != floor(d)) return lube_error(I, "range index must be a whole number");
        double k = d < 0 ? d + count : d;
        if (!(k >= 0 && k < count)) return lube_error(I, "range index %g is out of range (length %g)", d, count);
        return NUM_VAL(r->start + k * r->step);
    }
    if (IS_BYTES(obj)) {
        ObjBytes *b = AS_BYTES(obj);
        return list_index(I, idx, b->len, "bytes", &i) ? NUM_VAL(b->data[i]) : ERR_VAL;
    }
    return lube_error(I, "cannot index into %s", a_type(obj));
}

static Value component_member(Interp *I, ObjComponent *c, ObjString *name);

static Value field_get(Interp *I, Value obj, ObjString *name) {
    Value r;
    if (IS_FOREIGN(obj) && lube_foreign_property(I, obj, name, &r)) return r;  /* a plugin's: a.shape */
    if (IS_COMPONENT(obj)) return component_member(I, AS_COMPONENT(obj), name);  /* onclick: c.toggle */
    if (IS_MONEY(obj)) return lube_money_field(I, obj, name);
    if (IS_STROBJ(obj)) {
        return lube_string_property(I, obj, name);
    }
    if (IS_STRING(obj))
        return lube_error(I, "a str has no properties (it is plain text): use len(x) for its length, or "
                              "String(x).%s", name->chars);
    if (IS_CLASS(obj) && !strcmp(name->chars, "name")) return retain(OBJ_VAL(AS_CLASS(obj)->name));
    if (IS_PACKAGE(obj)) return package_member(I, obj, name);
    if (IS_MAP(obj)) {
        ObjMap *m = AS_MAP(obj);
        Value v;
        if (!map_key_ok(m, OBJ_VAL(name))) return bad_key(I, m, OBJ_VAL(name));
        if (map_get(m, OBJ_VAL(name), &v)) return retain(v);
        if (m->klass) return lube_error(I, "%s has no field '%s'", a_type(obj), name->chars);
        return lube_error(I, "the %s has no field '%s' (check with has() first, or use get(%s, key, default))",
                        m->dict ? "map" : "object", name->chars, m->dict ? "map" : "object");
    }
    Value f;
    if (map_get(I->globals, OBJ_VAL(name), &f) && IS_CALLABLE(f))
        return lube_error(I, "%s has no field '%s' (to call the function, write .%s())", a_type(obj),
                        name->chars, name->chars);
    return lube_error(I, "%s has no field '%s'", a_type(obj), name->chars);
}

/* ======================================================================
 * Places: the target of an assignment or of `&`, such as x, x[i] or x.a[2].
 * The path's keys are evaluated once; every later step only walks data and
 * runs no user code, so pointers into containers stay valid while in use.
 * ==================================================================== */

static void place_free(Place *p) {
    for (int i = 0; i < p->nkeys; i++) release(p->keys[i]);
    p->nkeys = 0;
}



/* May this code change the place's root variable? 0 = yes, -1 = error. */
LUBE_NOINLINE static int check_changeable(Interp *I, Place *p) {
    const VarRef *r = p->root;
    Origin origin;
    switch (r->kind) {
    case VAR_LOCAL:
        if (r->changeable && !(r->origin == ORIGIN_THIS && I->frame->this_locked)) return 0;
        origin = r->origin;
        break;
    case VAR_GLOBAL:
        if (!global_origin_ref(I, r, &origin)) return 0;
        break;
    case VAR_CAPTURE:
        origin = ORIGIN_CAPTURE;
        break;
    default:
        origin = ORIGIN_SELF;
        break;
    }
    cannot_change(I, lube_shown(r->name), origin);
    return -1;
}

/* Like check_changeable, without raising an error. */
static int place_changeable(Interp *I, Place *p) {
    const VarRef *r = p->root;
    Origin origin;
    if (r->kind == VAR_LOCAL) return r->changeable && !(r->origin == ORIGIN_THIS && I->frame->this_locked);
    return r->kind == VAR_GLOBAL && !global_origin_ref(I, r, &origin);
}

static Value *root_slot(Interp *I, Place *p) {
    const VarRef *r = p->root;
    if (r->kind == VAR_LOCAL) return &SLOT(r->index);
    int failed;
    Value *g = global_ref(I, r, &failed);
    if (failed) return NULL;
    if (!g) {
        lube_error(I, "undefined variable '%s' (declare it first with 'var %s = ...')", lube_shown(r->name),
                 lube_shown(r->name));
        return NULL;
    }
    if (g->type == VAL_LENT) {
        lube_error(I, IN_USE, lube_shown(r->name));
        return NULL;
    }
    return g;
}

/* Walks the path from *slot, giving each container along the way a single
 * owner first (copy-on-write). Returns the final slot, or NULL on error.
 * With `made` (for `=`), a missing last map key is created holding nothing,
 * and *made is set to that map so a failed assignment can remove the key
 * again (no "nothing" may stay behind in a value). */
static Value *navigate(Interp *I, Value *slot, Place *p, ObjMap **made, int *elem, const Value **holder,
                       const TypeDesc **ftype) {
    *elem = TT_ANY;  /* the type the container holding the final slot accepts... */
    *holder = NULL;  /* ...and that container (its type names it in messages) */
    *ftype = NULL;   /* ...or, for a field of an instance, that field's declared type */
    for (int i = 0; i < p->nkeys; i++) {
        Value key = p->keys[i];
        *ftype = NULL;  /* (it describes the last step only: a list in a field holds anything it takes) */
        if (IS_TUPLE(key) && !IS_FOREIGN(*slot)) {
            one_index(I, *slot, key);
            return NULL;
        }
        if (p->is_field[i] && !IS_MAP(*slot)) {
            if (IS_STROBJ(*slot))
                lube_error(I, "the properties of a String are read-only");
            else
                lube_error(I, "cannot set field '%s' on %s (only objects have fields)", AS_STRING(key)->chars,
                         a_type(*slot));
            return NULL;
        }
        if (IS_LIST(*slot)) {
            size_t k;
            if (!list_index(I, key, AS_LIST(*slot)->count, "list", &k)) return NULL;
            make_unique(slot);
            *elem = AS_LIST(*slot)->elem;
            *holder = slot;
            slot = &AS_LIST(*slot)->items[k];
        } else if (IS_INSTANCE(*slot) && IS_STRING(key) && field_slot(AS_MAP(*slot), AS_STRING(key)) >= 0) {
            make_unique(slot);  /* a field of an instance: no hashing (see field_slot) */
            ObjMap *m = AS_MAP(*slot);
            int f = field_slot(m, AS_STRING(key));
            *elem = m->vtype;
            *holder = slot;
            *ftype = &m->klass->field_types[f];
            slot = &m->entries[f].value;
        } else if (IS_MAP(*slot)) {
            if (!map_key_ok(AS_MAP(*slot), key)) {
                bad_key(I, AS_MAP(*slot), key);
                return NULL;
            }
            make_unique(slot);
            *elem = AS_MAP(*slot)->vtype;
            *holder = slot;
            ObjMap *m = AS_MAP(*slot);
            if (m->klass) {  /* an instance: only its declared fields exist */
                ObjClass *c = m->klass;
                int f = -1;
                for (int j = 0; j < c->nfields && f < 0 && IS_STRING(key); j++)
                    if (keys_same(OBJ_VAL(c->field_names[j]), key)) f = j;
                if (f < 0) {
                    if (IS_STRING(key)) lube_error(I, "%s has no field '%s'", with_article(c->name->chars), AS_STRING(key)->chars);
                    else lube_error(I, "%s has fields named by text, not %s", with_article(c->name->chars), a_type(key));
                    return NULL;
                }
                *ftype = &c->field_types[f];
            }
            Value *child = map_find(m, key);
            if (!child && IS_NUM(key) && key.as.number != key.as.number) {
                lube_error(I, "nan cannot be a key: it is not equal to itself, so it would never be found again");
                return NULL;
            }
            if (!child) {
                if (i < p->nkeys - 1 || !made) {
                    ObjString *k = value_repr(key);
                    const char *what = m->dict ? "map" : "object";
                    if (p->is_field[i])
                        lube_error(I, "the %s has no field '%s' (add it first with an assignment)", what,
                                 AS_STRING(key)->chars);
                    else
                        lube_error(I, "the %s has no key %s (add it first with an assignment)", what, k->chars);
                    release(OBJ_VAL(k));
                    return NULL;
                }
                child = map_slot(m, key);
                *made = m;
            }
            slot = child;
        } else if (IS_TEXT(*slot)) {
            lube_error(I, "text cannot be changed in place; build new text instead");
            return NULL;
        } else if (IS_FOREIGN(*slot) && lube_foreign_iterable(*slot)) {  /* (x[i][j] = v: x[i] is not a place) */
            lube_error(I, "cannot change a part of an element of %s: change the element itself (x[i, j] = v)",
                       a_type(*slot));
            return NULL;
        } else {
            lube_error(I, "cannot change an element of %s", a_type(*slot));
            return NULL;
        }
    }
    return slot;
}

/* Lends a place: moves its value out (into *out) and leaves the root empty
 * until place_restore. When the path has keys, the root value is kept in
 * *holder, privately, with a hole where the lent part was. */
LUBE_NOINLINE static int place_take(Interp *I, Place *p, Value *holder, Value *out) {
    Value *root = root_slot(I, p);
    if (!root) return -1;
    Value empty = p->root->kind == VAR_GLOBAL ? LENT_VAL : NIL_VAL;
    int elem;
    const Value *container;
    const TypeDesc *ftype;
    if (p->nkeys == 0) {
        *out = *root;
        *root = empty;
        *holder = NIL_VAL;
        return 0;
    }
    Value h = *root;  /* the root's reference moves into h */
    Value *slot = navigate(I, &h, p, NULL, &elem, &container, &ftype);  /* never lend a missing key */
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
    const VarRef *r = p->root;
    int failed;
    Value *root = r->kind == VAR_LOCAL ? &SLOT(r->index) : global_ref(I, r, &failed);
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
static Value place_get(Interp *I, Place *p) {
    Value v = get_var(I, p->root);
    for (int i = 0; i < p->nkeys && !IS_ERR(v); i++) {
        Value next = p->is_field[i] ? field_get(I, v, AS_STRING(p->keys[i])) : index_get(I, v, p->keys[i]);
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
    if (d->is_ctor && d->owner) return d->owner->name->chars;  /* Dog(...) */
    return d->name ? d->name->chars : "<lambda>";
}

/* The declared type a written type stands for. A class is looked up by
 * name when the function runs (classes are globals that never change). */
static int ann_desc(Interp *I, const TypeAnn *t, TypeDesc *out) {
    *out = t->d;
    if (t->d.kind != TD_CLASS) return 1;
    ObjString *name = t->cls;
    Value *c = map_find(I->globals, OBJ_VAL(name));
    if (!c || !IS_CLASS(*c)) {
        lube_error(I, "'%s' is not a type or a class", lube_shown(name));
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
    const TypeAnn *t = d->types && d->types[i].set ? &d->types[i] : NULL;
    if (!t || (t->d.kind == TD_NUM && IS_NUM(*v))) {  /* untyped, or the common typed case */
        if (d->modes[i] != PARAM_LET) I->decl[slot] = t ? t->d : desc_of(*v);  /* (only a var or & changes) */
        return 1;
    }
    I->decl[slot] = desc_of(*v);
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
    if (d->ret.set && d->ret.d.kind == TD_NUM && IS_NUM(result)) return result;  /* the common typed case */
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




/* Calls fn from C code with the arguments [self] + argv (self only when
 * has_self). Borrows them. A method called this way gets a `this` that it
 * cannot change, as for a method called on a value not in a variable. */
static Value call_from_c(Interp *I, Value fn, int has_self, Value self, int argc, Value *argv) {
    int n = argc + has_self;
    if (!stack_room(I, n)) return lube_error(I, "stack overflow: too much recursion");
    int base = I->sp;
    if (has_self) I->stack[base] = retain(self);
    for (int i = 0; i < argc; i++) I->stack[base + has_self + i] = retain(argv[i]);
    I->sp = base + n;
    I->this_locked_next = has_self;
    Value r = call_value(I, fn, n, base);
    I->this_locked_next = 0;
    pop_to(I, base);
    return r;
}

/* Calls a function from C code (the standard library). Borrows argv. */
Value lube_call(Interp *I, Value fn, int argc, Value *argv) {
    if (IS_FUNCTION(fn)) {
        FuncDecl *d = AS_FUNCTION(fn)->decl;
        for (int i = 0; d->has_ref && i < d->nparams; i++)
            if (d->modes[i] == PARAM_REF)
                return lube_error(I, "%s() changes its argument '%s' with &, so it cannot be used as a callback",
                                  callee_name(fn), d->params[i]->chars);
    } else if (IS_NATIVE(fn) && AS_NATIVE(fn)->changes_first) {
        return lube_error(I, "%s() changes its argument, so it cannot be used as a callback", callee_name(fn));
    }
    return call_from_c(I, fn, 0, NIL_VAL, argc, argv);
}

/* Calls a method of an instance from C code, with `this` a copy of self. */
Value lube_call_method(Interp *I, Value method, Value self, int argc, Value *argv) {
    return call_from_c(I, method, 1, self, argc, argv);
}

typedef struct Loan {
    int pos;                        /* argument position */
    Place place;
    Value holder;
    int lent;
    ObjString *watched;             /* while lent: the global var an `on` block watches, or NULL */
    const VarRef *lwatched;         /* ...or the local var */
    Value before;                   /* ...and its value before the call */
} Loan;

/* ======================================================================
 * Reactivity: `on score (old, new) { ... }` runs when the var score
 * changes, right after the assignment or call that changed it. A watched
 * var's value from before is kept while it changes (copy-on-write makes the
 * change copy it), and compared with the new one afterwards.
 *
 * Top-level blocks are functions in I->watchers, for globals. A block in a
 * function (or a block of code) is in I->fwatch while it is in scope: it
 * runs on that function's frame, as a frame of its own with the same base,
 * and code that changes a local it watches checks for it (OP_LWATCH_*).
 * ==================================================================== */

static Value run_frame_block(Interp *I, int i, Value old, Value now);

/* The value of an `on` item's field path in v (borrowed), or NIL_VAL. */
static Value item_value(const OnItem *it, Value v) {
    for (int i = 0; i < it->npath && !IS_NIL(v); i++) {
        Value next = NIL_VAL;
        if (IS_MAP(v)) map_get(AS_MAP(v), OBJ_VAL(it->path[i]), &next);
        v = next;
    }
    return v;
}

/* Does a change of a variable (a global by name, else a local by slot) from
 * before to after concern these items? Sets the watched value's old and
 * new (borrowed). A field that is missing before or after is no change. */
static int items_hit(const OnItem *items, int n, ObjString *name, int slot, Value before, Value after, Value *old,
                     Value *now) {
    int hit = 0;
    for (int j = 0; j < n; j++) {
        const OnItem *it = &items[j];
        if (name ? it->root.kind != VAR_GLOBAL || it->root.name != name
                 : it->root.kind != VAR_LOCAL || it->root.index != slot)
            continue;
        Value o = item_value(it, before), w = item_value(it, after);
        if (!IS_NIL(o) && !IS_NIL(w) && !values_equal(o, w)) {
            *old = o;
            *now = w;
            hit = 1;
        }
    }
    return hit;
}

/* Is a frame's `on` block still in scope (its hidden slot holds it)? */
static int fw_live(Interp *I, const FrameWatcher *w) {
    Value t = I->stack[w->base + w->od->token];
    return t.type == VAL_OBJ && t.as.obj == w->token.as.obj;
}

static int listed(const OnList *l, const OnDesc *od) {
    for (int i = 0; i < l->n; i++)
        if (l->ons[i] == od) return 1;
    return 0;
}

int lube_global_watched(Interp *I, ObjString *name);

/* `name` when an `on` block watches the global var of that name; else NULL. */
static ObjString *watched_name(Interp *I, ObjString *name) {
    for (int i = 0; i < I->nwatchers; i++)
        for (int j = 0; j < I->watchers[i].nnames; j++)
            if (I->watchers[i].items[j].root.name == name) return name;
    for (int i = 0; i < I->nfwatch; i++)
        for (int j = 0; j < I->fwatch[i].od->count; j++)
            if (I->fwatch[i].od->items[j].root.kind == VAR_GLOBAL && I->fwatch[i].od->items[j].root.name == name &&
                fw_live(I, &I->fwatch[i]))
                return name;
    return NULL;
}

/* Is the global `name` watched by an `on` block? (native.c: a loop's machine code may not change it) */
int lube_global_watched(Interp *I, ObjString *name) { return (I->nwatchers || I->nfwatch) && watched_name(I, name); }

/* Runs the `on` blocks for `name` (except one already running: its own
 * changes do not run it again). Returns NIL_VAL, or an error a block raised. */
static Value run_watchers(Interp *I, ObjString *name, Value before, Value after) {
    Value old, now;
    for (int i = 0; i < I->nwatchers; i++) {  /* by index: a block may add more */
        if (I->watchers[i].running ||
            !items_hit(I->watchers[i].items, I->watchers[i].nnames, name, 0, before, after, &old, &now))
            continue;
        Value fn = retain(I->watchers[i].fn);
        Value args[2] = {old, now};
        I->watchers[i].running = 1;
        Value r = lube_call(I, fn, AS_FUNCTION(fn)->decl->nparams, args);
        I->watchers[i].running = 0;
        release(fn);
        if (IS_ERR(r)) return r;
        release(r);
    }
    for (int i = 0; i < I->nfwatch; i++) {  /* then those in functions, outermost first */
        if (!items_hit(I->fwatch[i].od->items, I->fwatch[i].od->count, name, 0, before, after, &old, &now)) continue;
        Value r = run_frame_block(I, i, old, now);
        if (IS_ERR(r)) return r;
    }
    return NIL_VAL;
}

/* After a watched global may have changed: runs its `on` blocks if it did.
 * Takes over `result`, which is returned unless a block raises an error. */
static Value after_change(Interp *I, ObjString *name, Value before, Value result) {
    Value *now = map_find(I->globals, OBJ_VAL(name));
    if (!now || IS_NIL(before) || values_equal(before, *now)) return result;
    Value after = retain(*now);
    Value e = run_watchers(I, name, before, after);
    release(after);
    if (!IS_ERR(e)) return result;
    release(result);
    return e;
}

/* A local var of the running frame that blocks in `list` watch: its value
 * (a new reference) if one of them is in scope, to compare after a change;
 * else NIL_VAL. */
static Value local_before(Interp *I, int slot, const OnList *list) {
    int base = I->frame->base;
    for (int i = 0; i < I->nfwatch; i++)
        if (I->fwatch[i].base == base && listed(list, I->fwatch[i].od) && fw_live(I, &I->fwatch[i]))
            return retain(I->stack[base + slot]);
    return NIL_VAL;
}

/* After a watched local may have changed (like after_change). */
static Value after_local(Interp *I, int slot, const OnList *list, Value before, Value result) {
    int base = I->frame->base;
    if (values_equal(before, I->stack[base + slot])) return result;
    Value after = retain(I->stack[base + slot]), old, now;
    for (int i = 0; i < I->nfwatch; i++) {
        if (I->fwatch[i].base != base || !listed(list, I->fwatch[i].od) ||
            !items_hit(I->fwatch[i].od->items, I->fwatch[i].od->count, NULL, slot, before, after, &old, &now))
            continue;
        Value r = run_frame_block(I, i, old, now);
        if (IS_ERR(r)) {
            release(after);
            release(result);
            return r;
        }
    }
    release(after);
    return result;
}

/* The value of a watched global, kept to compare with after a change. */
static Value value_before(Interp *I, ObjString *name) {
    Value *g = name ? map_find(I->globals, OBJ_VAL(name)) : NULL;
    return g ? retain(*g) : NIL_VAL;
}

static int lent_at(Loan *loans, int n, int pos) {
    for (int j = 0; j < n; j++)
        if (loans[j].pos == pos) return 1;
    return 0;
}

/* `&` must be used exactly for the parameters that change. */
LUBE_NOINLINE static int check_loans(Interp *I, Value callee, int argc, Loan *loans, int nloans, int this_by_value) {
    const char *name = callee_name(callee);
    if (IS_CLASS(callee)) {
        if (nloans == 0) return 0;
        lube_error(I, "%s() does not take & arguments", name);
        return -1;
    }
    if (IS_NATIVE(callee)) {
        ObjNative *nf = AS_NATIVE(callee);
        for (int j = 0; j < nloans; j++) {
            if (!(nf->changes_first && loans[j].pos == 0)) {
                lube_error(I, "%s() does not change argument %d, so it does not take '&'", name, loans[j].pos + 1);
                return -1;
            }
        }
        if (nf->changes_first && argc > 0 && !lent_at(loans, nloans, 0)) {
            lube_error(I, "%s() changes its first argument, so pass it with &: %s(&x, ...) or x.%s(...)", name, name,
                     name);
            return -1;
        }
        return 0;
    }
    FuncDecl *d = AS_FUNCTION(callee)->decl;
    for (int j = 0; j < nloans; j++) {
        int pos = loans[j].pos;
        if (pos < d->nparams && d->modes[pos] != PARAM_REF) {
            lube_error(I, "%s() does not change '%s', so it does not take '&'", name, d->params[pos]->chars);
            return -1;
        }
    }
    for (int i = this_by_value ? 1 : 0; i < argc && i < d->nparams; i++) {
        if (d->modes[i] == PARAM_REF && !lent_at(loans, nloans, i)) {
            lube_error(I, "%s() changes '%s', so pass it with &: %s(&x)", name, d->params[i]->chars, name);
            return -1;
        }
    }
    return 0;
}

static int same_root(Place *a, Place *b) {
    const VarRef *x = a->root, *y = b->root;
    if (x->kind != y->kind) return 0;
    return x->kind == VAR_LOCAL ? x->index == y->index : x->name == y->name;
}


/* Does a call to f need & somewhere (so a call without any must be refused)? */
static int takes_ref(Value f) {
    if (IS_NATIVE(f)) return AS_NATIVE(f)->changes_first;
    return IS_FUNCTION(f) && AS_FUNCTION(f)->decl->has_ref;
}

static int takes_first_by_ref(Value f) {
    if (IS_NATIVE(f)) return AS_NATIVE(f)->changes_first;
    if (IS_CLASS(f)) return 0;  /* x.Point() is Point(x) */
    FuncDecl *d = AS_FUNCTION(f)->decl;
    return d->nparams > 0 && d->modes[0] == PARAM_REF;
}



/* place op= *rhs the quick way, for the common cases: the root a var (a
 * global one not lent out), then lists (a whole index in range) and
 * instances (a declared field), each held by nothing else (so nothing needs
 * copying first), and a Number (or anything, into an untyped list). The
 * keys are on top of the stack. Returns 1 when done, with *rhs the value
 * stored (the slot took it over); 0 to go the general way. */
static int fast_assign(Interp *I, PlaceDesc *pd, TokenType op, Value *rhs) {
    const VarRef *r = &pd->var;
    Value *slot = NULL;
    Origin origin;
    int failed;
    if (r->kind == VAR_LOCAL && r->changeable && !(r->origin == ORIGIN_THIS && I->frame->this_locked)) {
        slot = &SLOT(r->index);
    } else if (r->kind == VAR_GLOBAL) {
        Value *g = global_ref(I, r, &failed);
        if (g && g->type != VAL_LENT && !global_origin_ref(I, r, &origin)) slot = g;
    }
    if (!slot) return 0;
    const Value *keys = &I->stack[I->sp - pd->nkeys];
    int numeric = 1;  /* the final slot takes only a Number (a typed field), or anything (an untyped list) */
    for (int i = 0; i < pd->nkeys; i++) {  /* (only the last step's type matters: the others hold containers) */
        int last = i == pd->nkeys - 1;
        if (!IS_OBJ(*slot) || slot->as.obj->rc != 1) return 0;
        if (IS_MAP(*slot) && !AS_MAP(*slot)->klass) {  /* an Object or a Map: o.x = v, counts[w] += 1 */
            ObjMap *m = AS_MAP(*slot);
            Value key = keys[i], *child;
            if (!IS_STRING(key) && !(IS_NUM(key) && key.as.number == key.as.number)) return 0;
            if (last && op == T_ASSIGN && m->vtype == TT_ANY && m->ktype == TT_ANY) {
                slot = map_slot(m, key);  /* (a new key holds nothing until the store below, which cannot fail) */
                numeric = 0;
                continue;
            }
            child = map_find(m, key);
            if (!child || (last && m->vtype != TT_ANY && m->vtype != TT_NUMBER)) return 0;
            numeric = m->vtype == TT_NUMBER;
            slot = child;
            continue;
        }
        if (pd->is_field[i]) {
            int fs = IS_INSTANCE(*slot) ? field_slot(AS_MAP(*slot), AS_STRING(keys[i])) : -1;
            if (fs < 0 || (last && AS_MAP(*slot)->klass->field_types[fs].kind != TD_NUM)) return 0;
            slot = &AS_MAP(*slot)->entries[fs].value;
            numeric = 1;
        } else {
            double d = IS_NUM(keys[i]) ? keys[i].as.number : -1;
            if (!IS_LIST(*slot) || !(d >= 0 && d < (double)AS_LIST(*slot)->count && d == (double)(size_t)d)) return 0;
            int elem = AS_LIST(*slot)->elem;
            if (last && elem != TT_ANY && elem != TT_NUMBER) return 0;
            numeric = elem == TT_NUMBER;
            slot = &AS_LIST(*slot)->items[(size_t)d];
        }
    }
    Value v = *rhs;
    if (op == T_ASSIGN) {
        if (numeric && !IS_NUM(v)) return 0;
    } else if (!IS_NUM(*slot) || !IS_NUM(v) || !num_op(compound_base(op), slot->as.number, v.as.number, &v)) {
        return 0;
    }
    Value old = *slot;
    *slot = v;
    release(old);
    *rhs = v;
    return 1;
}

/* ======================================================================
 * Assignment: place op= rhs, with the place's keys already evaluated.
 * Takes over p's keys and rhs; returns the value assigned (a new
 * reference) or ERR_VAL.
 * ==================================================================== */

/* x[i] = v and x[i] op= v, x (in *slot) a plugin's object: copied first if shared, then its "[]=" (and
 * "[]" for op=). Takes rhs; returns the value assigned. */
static Value foreign_assign(Interp *I, Value *slot, Value idx, TokenType op, Value rhs) {
    Value r = rhs;
    if (op != T_ASSIGN) {
        Value old;
        if (!lube_foreign_index(I, *slot, idx, &old)) old = lube_error(I, "cannot index into %s", a_type(*slot));
        if (IS_ERR(old)) {
            release(rhs);
            return old;
        }
        r = binary_op(I, compound_base(op), old, rhs);
        release(old);
        release(rhs);
        if (IS_ERR(r)) return r;
    }
    if (lube_foreign_unshare(I, slot) < 0) {
        release(r);
        return ERR_VAL;
    }
    int done = lube_foreign_set(I, *slot, idx, r);
    if (!done) lube_error(I, "cannot change an element of %s", a_type(*slot));
    if (done <= 0) {
        release(r);
        return ERR_VAL;
    }
    return r;
}

LUBE_NOINLINE static Value do_assign(Interp *I, TokenType op, Place *place, Value rhs) {
#define p (*place)
    Value *slot = NULL;
    int elem = TT_ANY;
    const Value *holder = NULL;
    const TypeDesc *ftype = NULL;
    int whole = p.nkeys == 0;  /* assigning the variable itself */
    Value field = whole ? NIL_VAL : p.keys[p.nkeys - 1];
    ObjMap *made = NULL;  /* the map that got a new, still empty key */
    if (check_changeable(I, &p) == 0) {
        Value *root = root_slot(I, &p);
        if (root && !whole && !p.is_field[p.nkeys - 1] && (p.nkeys > 1 || IS_BYTES(*root) || IS_FOREIGN(*root))) {
            /* (b[i] = v: a byte of Bytes has no slot of its own, nor an element of a plugin's object) */
            Value *c = root;
            if (p.nkeys > 1) {
                p.nkeys--;
                c = navigate(I, root, &p, NULL, &elem, &holder, &ftype);
                p.nkeys++;
            }
            if (c && IS_BYTES(*c)) {
                size_t k;
                Value r = list_index(I, field, AS_BYTES(*c)->len, "bytes", &k) ? lube_bytes_assign(I, c, k, op, rhs)
                                                                                : (release(rhs), ERR_VAL);
                place_free(&p);
                return r;
            }
            if (c && IS_FOREIGN(*c)) {
                Value r = foreign_assign(I, c, field, op, rhs);
                place_free(&p);
                return r;
            }
            if (c) {
                Place last;
                last.root = p.root;
                last.keys[0] = field;  /* (borrowed from p) */
                last.is_field[0] = 0;
                last.nkeys = 1;
                slot = navigate(I, c, &last, op == T_ASSIGN ? &made : NULL, &elem, &holder, &ftype);
            }
        } else if (root) {
            slot = navigate(I, root, &p, op == T_ASSIGN ? &made : NULL, &elem, &holder, &ftype);
        }
    }
    if (!slot) {
        place_free(&p);
        release(rhs);
        return ERR_VAL;
    }
    /* the declared type the new value must have: a var's, or a class field's */
#define KEEP_TYPE(v)                                                                          \
    do {                                                                                      \
        if (whole) v = keep_type(I, p.root, v);                                            \
        else if (ftype && !(ftype->kind == TD_NUM && IS_NUM(v)))                            \
            v = keep_field_type(I, ftype, field, *holder, v);                               \
    } while (0)

    if (op == T_ASSIGN) {
        if (elem != TT_ANY) {  /* into a typed list or map */
            Value c = CHECK_ELEM(I, type_name(*holder), elem, rhs);
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
        if (IS_BYTES(*slot) && IS_BYTES(rhs)) {
            ObjBytes *b = AS_BYTES(*slot), *add = AS_BYTES(rhs);
            if (b->len + add->len > LUBE_MAX_BYTES) lube_out_of_memory();
            bytes_reserve(b, b->len + add->len);
            if (add->len) memcpy(b->data + b->len, add->data, add->len);
            b->len += add->len;
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
    Value r;
    if (IS_FOREIGN(*slot) && lube_foreign_inplace(I, bin, *slot, rhs, &r)) {}  /* x += b, x nobody else's */
    else if (!IS_NUM(*slot) || !IS_NUM(rhs) || !num_op(bin, slot->as.number, rhs.as.number, &r)) r = binary_op(I, bin, *slot, rhs);
    release(rhs);
    if (!IS_ERR(r) && elem != TT_ANY) {
        Value c = CHECK_ELEM(I, type_name(*holder), elem, r);
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
#undef p



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


/* ======================================================================
 * The virtual machine
 *
 * Each call to a Lube function gets a Frame (in chunks that never move) and
 * runs in the same loop as its caller, so recursion uses no C stack. A call
 * made from C (a callback from map(), a watcher, an import) starts an
 * "entry" frame and a nested loop that returns when that frame does.
 * ==================================================================== */

static void hot_check(Interp *I);

static void keep_arena(Interp *I, Arena *a) {
    if (I->narenas == I->cap_arenas) {
        I->cap_arenas = I->cap_arenas ? I->cap_arenas * 2 : 8;
        I->arenas = xrealloc(I->arenas, sizeof(Arena *) * (size_t)I->cap_arenas);
    }
    I->arenas[I->narenas++] = a;
}

void lube_keep_arena(Interp *I, Arena *a) { keep_arena(I, a); }

/* A closure of d made as if in frame f (the debugger's expressions). */
Value lube_closure_in(Interp *I, FuncDecl *d, Frame *f) {
    Frame *top = I->frame;
    I->frame = f;
    Value c = make_closure(I, d);
    I->frame = top;
    return c;
}

#define FRAME_CHUNK 256
typedef struct FrameChunk {
    Frame frames[FRAME_CHUNK];
} FrameChunk;

/* The frame after the newest (which the caller makes the newest: I->frame). */
static Frame *new_frame(Interp *I) {
    int k = VM_DEPTH(I);
    if (k / FRAME_CHUNK == I->nchunks) {
        I->chunks = xrealloc(I->chunks, sizeof(FrameChunk *) * (size_t)(I->nchunks + 1));
        FrameChunk *c = I->chunks[I->nchunks++] = xmalloc(sizeof(FrameChunk));
        for (int i = 0; i < FRAME_CHUNK; i++) {  /* what never changes of a frame: where it is */
            c->frames[i].index = k + i;
            c->frames[i].last = i == FRAME_CHUNK - 1 || k + i + 1 >= LUBE_MAX_FRAMES;
        }
    }
    return &I->chunks[k / FRAME_CHUNK]->frames[k % FRAME_CHUNK];
}

static Value execute(Interp *I);
static int check_items(Interp *I, OnDesc *od);

/* A frame whose end has more to do than give its slots back (see Frame.slow_ret): execute()'s loop leaves it to
 * op_return. */
#if VM_SLIM_CALL
#define FRAME_SLOW_RET(f) ((f)->slow_ret = 1)
#define FRAME_LENT(f) ((f)->lent != NULL)
#else
#define FRAME_SLOW_RET(f) ((void)0)
#define FRAME_LENT(f) 0
#endif

/* Fills a new frame (the newest: its pending calls start where the Interp's are now). */
LUBE_INLINE void frame_init(Interp *I, Frame *f, Frame *prev, ObjFunction *fn, FuncDecl *d, const char *name, int line,
                            int base, int argc, int call_base, int opbase) {
    f->prev = prev;
#if VM_SLIM_CALL
    (void)name;
    (void)line;
    f->slow_ret = fn && (d->ret.set || d->strict);  /* (what it returns is checked; whoever makes it an entry frame, a
                                                      constructor's or a block's, or gives it loans, says so too) */
#else
    f->name = name;
    f->src = d->src;
    f->line = line;
#endif
    f->base = base;
    f->slots = I->stack + base;
    f->fn = fn;
    f->this_locked = 0;
    f->proto = d;
    f->pc = d->code.code;
    f->argc = argc;
    f->call_base = call_base;
    f->pending = I->npending;
    f->construct = 0;
    f->entry = 0;
    f->block = 0;
    f->opbase = opbase;
    f->loans = NULL;
    f->nloans = 0;
    f->dbg_line = DBG_NO_LINE;
#if VM_SLIM_CALL
    f->lent = NULL;
#endif
}

/* Calls d's machine code (lube --build --native) with the argc arguments at
 * stack[base...]: 1 with the result in *out, or 0 when the VM must run the
 * call itself (arguments of other types, which it reports; an error the
 * machine code leaves to it; calls too deep; the debugger). */
#define NATIVE_DEPTH 20000  /* (C stack enough for this many) */
static int native_call(Interp *I, FuncDecl *d, int base, int argc, Value *out) {
    const char *sig = d->native->sig;
    double args[8], result;
    int left = LUBE_MAX_FRAMES - VM_DEPTH(I) < NATIVE_DEPTH ? LUBE_MAX_FRAMES - VM_DEPTH(I) : NATIVE_DEPTH;
    if (d->native->fnval) {  /* a value back (its own: the VM's now) */
        return !d->native->fnval(I, &I->stack[base], out, left);
    } else if (d->native->fnv) {  /* records: its wrapper takes the arguments as they are */
        if (d->native->fnv(I, &I->stack[base], &result, left)) return 0;
    } else {
        for (int i = 0; i < argc; i++) {
            Value v = I->stack[base + i];
            if (sig[i] == 'N' && IS_NUM(v)) args[i] = v.as.number;
            else if (sig[i] == 'B' && IS_BOOL(v)) args[i] = v.as.boolean;
            else return 0;
        }
        if (d->native->fn(args, &result, left)) return 0;
    }
    char ret = sig[argc + 1];
    *out = ret == 'N' ? NUM_VAL(result) : ret == 'B' ? BOOL_VAL(result != 0) : NIL_VAL;
    return 1;
}

static int call_native(Interp *I, FuncDecl *d, int base, int argc, Value *out) {
    if (I->dbg || argc != d->nparams) return 0;
    if (I->native_off) {  /* (see Interp.native_off) */
        if (VM_DEPTH(I) >= I->native_off) return 0;
        I->native_off = 0;
    }
    if (native_call(I, d, base, argc, out)) return 1;
    I->native_off = VM_DEPTH(I) + 1;
    return 0;
}

/* on x { ... } in a function: registers the block for the running frame and
 * puts it in its hidden slot (cleared, as every slot, when the code around
 * it ends). Blocks of this frame already out of scope are dropped first. */
static int add_frame_watcher(Interp *I, OnDesc *od) {
    if (check_items(I, od) < 0) return -1;
    int keep = 0, first = I->nfwatch;
    while (first > 0 && I->fwatch[first - 1].depth == VM_DEPTH(I)) first--;
    for (int i = first; i < I->nfwatch; i++) {
        if (fw_live(I, &I->fwatch[i])) I->fwatch[first + keep++] = I->fwatch[i];
        else release(I->fwatch[i].token);
    }
    I->nfwatch = first + keep;
    if (I->nfwatch == I->cap_fwatch) {
        I->cap_fwatch = I->cap_fwatch ? I->cap_fwatch * 2 : 8;
        I->fwatch = xrealloc(I->fwatch, sizeof(FrameWatcher) * (size_t)I->cap_fwatch);
    }
    FrameWatcher *w = &I->fwatch[I->nfwatch++];
    w->od = od;
    w->base = I->frame->base;
    w->depth = VM_DEPTH(I);
    w->token = OBJ_VAL(new_function(od->func, 0));
    w->running = 0;
    set_slot(I, od->token, retain(w->token));
    FRAME_SLOW_RET(I->frame);  /* (its `on` blocks end with it: see leave) */
    return 0;
}

static Value run_block_frame(Interp *I, FrameWatcher *w, int i);

static void set_slot_at(Interp *I, int at, Value v) {
    Value prev = I->stack[at];
    I->stack[at] = v;
    release(prev);
}

/* Runs frame watcher i's block with the argc arguments in argv (borrowed; as many as it has parameters): on the
 * frame it belongs to, as a frame of its own with the same base, so it sees that frame's variables. Returns what
 * the block returned (NIL_VAL for an `on` block that does not run now), or ERR_VAL. */
static Value run_block_args(Interp *I, int i, const Value *argv) {
    FrameWatcher *w = &I->fwatch[i];
    OnDesc *od = w->od;
    FuncDecl *d = od->func;
    int base = w->base;
    if (w->running || !fw_live(I, w)) {
        if (od->kind == ON_WATCH) return NIL_VAL;
        return lube_error(I, "%s() is already running: it cannot call itself", d->name->chars);
    }
    for (int k = 0; k < od->nuses; k++)
        if (IS_NIL(I->stack[base + od->uses[k].slot]))
            return lube_error(I, "the '%s' block cannot run now: '%s' is lent out with & to a call that has not returned",
                              d->name->chars, od->uses[k].name->chars);
    if (VM_DEPTH(I) >= LUBE_MAX_FRAMES || I->sp + d->code.maxstack > LUBE_STACK_MAX)
        return lube_error(I, "stack overflow: too much recursion");
    Value r = NIL_VAL;
    for (int k = 0; k < d->nparams && !IS_ERR(r); k++) {
        Value v = retain(argv[k]);
        int slot = base + od->param_slot[0] + k;
        if (check_param(I, d, d->name->chars, k, &v, slot)) set_slot_at(I, slot, v);
        else r = ERR_VAL;
        if (IS_ERR(r)) release(v);
    }
    if (!IS_ERR(r)) r = run_block_frame(I, w, i);
    Value *own = &I->stack[base + od->first];
    for (int k = 0; k < od->nslots; k++) {
        Value v = own[k];
        own[k] = NIL_VAL;
        release(v);
    }
    return r;
}

/* Runs frame watcher i's block with (old, new). Returns NIL_VAL or ERR_VAL (a result it returned is dropped). */
static Value run_frame_block(Interp *I, int i, Value old, Value now) {
    Value args[2] = {old, now};
    Value r = run_block_args(I, i, args);
    if (IS_ERR(r)) return r;
    release(r);
    return NIL_VAL;
}

/* Pushes the block's frame and runs it. */
static Value run_block_frame(Interp *I, FrameWatcher *w, int i) {
    FuncDecl *d = w->od->func;
    Frame *owner = &I->chunks[(w->depth - 1) / FRAME_CHUNK]->frames[(w->depth - 1) % FRAME_CHUNK];
    Frame *f = new_frame(I);
    frame_init(I, f, I->frame, owner->fn /* for the function's captures */, d, d->name->chars, d->line, w->base, 0, I->sp,
               I->sp);
    f->this_locked = owner->this_locked;
    f->entry = 1;
    f->block = 1;
    FRAME_SLOW_RET(f);
    I->frame = f;
    w->running = 1;
    Value r = execute(I);
    I->fwatch[i].running = 0;  /* (the array may have moved) */
    return r;
}

/* ======================================================================
 * Components (see ObjComponent in lube.h). A component's frame is on the
 * stack while its body runs, and again while one of its blocks runs; in
 * between, its slots and blocks are kept in the component.
 * ==================================================================== */

void lube_cell_release(CompCell *cell) {
    if (--cell->rc == 0) free(cell);
}

static ObjComponent *new_component(FuncDecl *d) {
    ObjComponent *c = (ObjComponent *)alloc_object(sizeof(ObjComponent), OBJ_COMPONENT);
    c->decl = d;
    c->cell = xmalloc(sizeof(CompCell));
    c->cell->rc = 1;
    c->cell->comp = c;
    c->slots = NULL;
    c->types = NULL;
    c->blocks = NULL;
    c->nblocks = c->capblocks = 0;
    c->depth = 0;
    c->left = 0;
    return c;
}

/* Its frame (at base, the newest at its depth) gives its slots and blocks to c. The caller holds c. */
static void component_keep(Interp *I, ObjComponent *c, int base) {
    FuncDecl *d = c->decl;
    int first = I->nfwatch;
    while (first > 0 && I->fwatch[first - 1].depth == c->depth) first--;
    for (int i = first; i < I->nfwatch; i++) {  /* (all in scope: its view, added last, dropped those that were not) */
        FrameWatcher *w = &I->fwatch[i];
        if (c->nblocks == c->capblocks) {
            c->capblocks = c->capblocks ? c->capblocks * 2 : 4;
            c->blocks = xrealloc(c->blocks, sizeof(CompBlock) * (size_t)c->capblocks);
        }
        c->blocks[c->nblocks].od = w->od;
        c->blocks[c->nblocks++].token = w->token;
    }
    I->nfwatch = first;
    if (!c->slots) {
        c->slots = xmalloc(sizeof(Value) * (size_t)d->nslots);
        c->types = xmalloc(sizeof(TypeDesc) * (size_t)d->nslots);
    }
    for (int i = 0; i < d->nslots; i++) {
        c->slots[i] = I->stack[base + i];
        c->types[i] = I->decl[base + i];
        I->stack[base + i] = NIL_VAL;
    }
    Value self = c->slots[d->comp_slot];
    c->slots[d->comp_slot] = NIL_VAL;
    c->depth = 0;
    release(self);
}

/* c's frame back on the stack, the newest, with its blocks: NULL (an error raised) when there is no room. */
static Frame *component_enter(Interp *I, ObjComponent *c) {
    FuncDecl *d = c->decl;
    int base = I->sp;
    if (VM_DEPTH(I) + 1 >= LUBE_MAX_FRAMES || base + d->nslots + d->code.maxstack > LUBE_STACK_MAX) {
        lube_error(I, "stack overflow: too much recursion");
        return NULL;
    }
    for (int i = 0; i < d->nslots; i++) {
        I->stack[base + i] = c->slots[i];
        I->decl[base + i] = c->types[i];
        c->slots[i] = NIL_VAL;
    }
    I->stack[base + d->comp_slot] = retain(OBJ_VAL(c));
    I->sp = base + d->nslots;
    Frame *f = new_frame(I);
    frame_init(I, f, I->frame, NULL, d, d->name->chars, d->line, base, 0, base, base + d->nslots);
    f->entry = 1;
    FRAME_SLOW_RET(f);
    I->frame = f;
    c->depth = VM_DEPTH(I);
    for (int i = 0; i < c->nblocks; i++) {
        if (I->nfwatch == I->cap_fwatch) {
            I->cap_fwatch = I->cap_fwatch ? I->cap_fwatch * 2 : 8;
            I->fwatch = xrealloc(I->fwatch, sizeof(FrameWatcher) * (size_t)I->cap_fwatch);
        }
        FrameWatcher *w = &I->fwatch[I->nfwatch++];
        w->od = c->blocks[i].od;
        w->base = base;
        w->depth = c->depth;
        w->token = c->blocks[i].token;
        w->running = 0;
    }
    c->nblocks = 0;
    return f;
}

/* Where c's block od is in I->fwatch (c's frame on the stack: its blocks are all there). */
static int component_block_at(Interp *I, ObjComponent *c, const OnDesc *od) {
    int i = I->nfwatch - 1;
    while (I->fwatch[i].depth != c->depth || I->fwatch[i].od != od) i--;
    return i;
}

/* c's block of this kind (and name, for a handler), or NULL. */
static const OnDesc *component_find(Interp *I, ObjComponent *c, int kind, ObjString *name) {
    int n = c->depth ? I->nfwatch : c->nblocks;
    for (int i = 0; i < n; i++) {
        const OnDesc *od = c->depth ? I->fwatch[i].od : c->blocks[i].od;
        if (c->depth && I->fwatch[i].depth != c->depth) continue;
        if (od->kind == kind && (!name || od->func->name == name)) return od;
    }
    return NULL;
}

/* Runs c's block od with argv (borrowed): on its frame, which is on the stack already (its body, or another of
 * its blocks, is running) or is brought back for it. Returns what the block returned, or ERR_VAL. */
static Value component_run(Interp *I, ObjComponent *c, const OnDesc *od, const Value *argv) {
    if (c->left) return lube_error(I, "this %s has left: its blocks are gone", c->decl->name->chars);
    retain(OBJ_VAL(c));
    Frame *own = c->depth ? NULL : component_enter(I, c);
    Value r = c->depth ? run_block_args(I, component_block_at(I, c, od), argv) : ERR_VAL;
    if (own) {
        int base = own->base;
        component_keep(I, c, base);
        pop_to(I, base);
        I->frame = own->prev;
        if (I->dbg) debug_left_frame(I);
    }
    release(OBJ_VAL(c));
    return r;
}

/* c.view(): what its body returned, worked out again. */
static Value component_view(Interp *I, ObjComponent *c) {
    if (c->left) return lube_error(I, "this %s has left: it has no view", c->decl->name->chars);
    const OnDesc *od = component_find(I, c, ON_VIEW, NULL);
    if (!od) return lube_error(I, "%s's view cannot be worked out while its body runs", c->decl->name->chars);
    return component_run(I, c, od, NULL);
}

/* c.leave(): its `on leave` blocks run, then its blocks are gone (it stays, with no view). */
static Value component_leave(Interp *I, ObjComponent *c) {
    if (c->left) return NIL_VAL;
    if (c->depth) return lube_error(I, "%s cannot leave while its body or one of its blocks runs", c->decl->name->chars);
    for (int i = 0; i < c->nblocks && !c->left; i++) {
        if (c->blocks[i].od->kind != ON_LEAVE) continue;
        Value r = component_run(I, c, c->blocks[i].od, NULL);
        if (IS_ERR(r)) return r;
        release(r);
    }
    c->left = 1;
    for (int i = 0; i < c->nblocks; i++) release(c->blocks[i].token);
    c->nblocks = 0;
    return NIL_VAL;
}

/* A handler (or c.view, c.leave) kept as a function: the component it belongs to, or NULL with an error raised
 * (it does not keep the component: see CompCell). */
static ObjComponent *bound_component(Interp *I) {
    ObjComponent *c = I->calling->cell->comp;
    if (!c) lube_error(I, "%s() belongs to a component that is gone", I->calling->name);
    return c;
}

static Value n_handler(Interp *I, int argc, Value *argv) {
    const OnDesc *od = I->calling->od;
    ObjComponent *c = bound_component(I);
    (void)argc;
    return c ? component_run(I, c, od, argv) : ERR_VAL;
}

static Value n_component_view(Interp *I, int argc, Value *argv) {
    ObjComponent *c = bound_component(I);
    (void)argc;
    (void)argv;
    return c ? component_view(I, c) : ERR_VAL;
}

static Value n_component_leave(Interp *I, int argc, Value *argv) {
    ObjComponent *c = bound_component(I);
    (void)argc;
    (void)argv;
    return c ? component_leave(I, c) : ERR_VAL;
}

/* c.toggle(...), c.view(), c.leave(): called as methods, with c first (so a component made for the call lasts
 * through it). */
static Value n_component_method(Interp *I, int argc, Value *argv) {
    const OnDesc *od = I->calling->od;
    const char *name = I->calling->name;
    int want = od ? od->func->nparams : 0;
    if (argc - 1 != want) return arity_error(I, name, want, want, argc - 1);
    ObjComponent *c = AS_COMPONENT(argv[0]);
    if (od) return component_run(I, c, od, argv + 1);
    return name[0] == 'v' ? component_view(I, c) : component_leave(I, c);
}

static Value bound_native(ObjComponent *c, const char *name, NativeFn fn, const OnDesc *od, int nargs) {
    ObjNative *n = new_native(name, fn, nargs, nargs, 0);
    n->cell = c->cell;
    c->cell->rc++;
    n->od = od;
    return OBJ_VAL(n);
}

/* c's handler `name`, or NULL with an error raised. */
static const OnDesc *component_handler(Interp *I, ObjComponent *c, ObjString *name) {
    if (c->left) {
        lube_error(I, "this %s has left: its blocks are gone", c->decl->name->chars);
        return NULL;
    }
    const OnDesc *od = component_find(I, c, ON_HANDLER, name);
    if (!od) lube_error(I, "%s has no handler '%s'", c->decl->name->chars, name->chars);
    return od;
}

/* c.name: its view or leave, or a handler of its, as a function. */
static Value component_member(Interp *I, ObjComponent *c, ObjString *name) {
    if (!strcmp(name->chars, "view")) return bound_native(c, "view", n_component_view, NULL, 0);
    if (!strcmp(name->chars, "leave")) return bound_native(c, "leave", n_component_leave, NULL, 0);
    const OnDesc *od = component_handler(I, c, name);
    return od ? bound_native(c, od->func->name->chars, n_handler, od, od->func->nparams) : ERR_VAL;
}

/* c.name(...): the function to call with c first. */
static Value component_method(Interp *I, ObjComponent *c, ObjString *name) {
    const OnDesc *od = NULL;
    if (strcmp(name->chars, "view") && strcmp(name->chars, "leave") && !(od = component_handler(I, c, name)))
        return ERR_VAL;
    ObjNative *n = new_native(od ? od->func->name->chars : name->chars, n_component_method, 1, -1, 0);
    n->od = od;
    return OBJ_VAL(n);
}

static PendingCall *new_pending(Interp *I) {
    if (I->npending == I->cap_pending) {
        I->cap_pending = I->cap_pending ? I->cap_pending * 2 : 16;
        I->pending = xrealloc(I->pending, sizeof(PendingCall) * (size_t)I->cap_pending);
    }
    PendingCall *p = &I->pending[I->npending++];
    p->place.nkeys = 0;
    return p;
}

static void pop_pending(Interp *I, int to) {
    while (I->npending > to) place_free(&I->pending[--I->npending].place);
}

/* Moves a place (only the keys it has: a Place is large). */
static void move_place(Place *dst, const Place *src) {
    dst->root = src->root;
    dst->nkeys = src->nkeys;
    for (int i = 0; i < src->nkeys; i++) {
        dst->keys[i] = src->keys[i];
        dst->is_field[i] = src->is_field[i];
    }
}

/* Loan arrays come and go with every call that lends: small ones are reused. */
#define LOAN_POOL 64
static Loan *new_loans(Interp *I, int n) {
    if (n <= 4 && I->nloan_pool > 0) return I->loan_pool[--I->nloan_pool];
    if (!I->loan_pool) I->loan_pool = xmalloc(sizeof(Loan *) * LOAN_POOL);
    return xmalloc(sizeof(Loan) * (size_t)(n < 4 ? 4 : n));
}

static void free_loans(Interp *I, Loan *loans, int n) {
    if (n <= 4 && I->nloan_pool < LOAN_POOL) I->loan_pool[I->nloan_pool++] = loans;
    else free(loans);
}

/* The global named by constant k, found through the code's cache (NULL
 * with *failed set when making it in an isolate raised an error). */
static Value *global_slot(Interp *I, Code *code, int k, int *failed) {
    ObjMap *g = I->globals;
    int at = code->gcache[k];
    *failed = 0;
    if (at >= 0 && (size_t)at < g->used && g->entries[at].live && g->entries[at].key.as.obj == code->consts[k].as.obj)
        return &g->entries[at].value;
    Value *v = lookup_global(I, AS_STRING(code->consts[k]), failed);
    if (v) code->gcache[k] = (int)((MapEntry *)(void *)((char *)v - offsetof(MapEntry, value)) - I->globals->entries);
    return v;
}

/* Returns what a call lent with & (each loan's value is in the argument
 * slot `args + pos`), then runs the `on` blocks of watched globals it
 * changed. Takes over the loans and `result`. */
static Value return_loans(Interp *I, int args, Loan *loans, int n, Value result) {
    for (int j = n - 1; j >= 0; j--) {  /* even after an error */
        if (loans[j].lent) {
            Value v = I->stack[args + loans[j].pos];
            I->stack[args + loans[j].pos] = NIL_VAL;
            place_restore(I, &loans[j].place, loans[j].holder, v);
        }
        place_free(&loans[j].place);
    }
    for (int j = 0; j < n; j++) {  /* `on` blocks run once everything lent is back */
        if (!loans[j].lent || IS_NIL(loans[j].before)) continue;
        const VarRef *lw = loans[j].lwatched;
        if (!IS_ERR(result))
            result = lw ? after_local(I, lw->index, lw->watch, loans[j].before, result)
                        : after_change(I, loans[j].watched, loans[j].before, result);
        release(loans[j].before);
    }
    free_loans(I, loans, n);
    return result;
}

/* Starts a call to fn with the argc arguments at stack[base...]: checks
 * them (errors belong to the caller), then pushes a frame. Returns it, or
 * NULL after raising an error. */
LUBE_INLINE Frame *push_frame(Interp *I, ObjFunction *fn, const char *name, int base, int argc);

static Frame *enter(Interp *I, ObjFunction *fn, int base, int argc) {
    FuncDecl *d = fn->decl;
    const char *name = callee_name(OBJ_VAL(fn));
    if (argc > d->nparams) {
        int required = d->nparams;
        while (required > 0 && d->has_default[required - 1]) required--;
        arity_error(I, name, required, d->nparams, argc);
        return NULL;
    }
    for (int i = argc; i < d->nparams; i++) {
        if (!d->has_default[i]) {
            lube_error(I, "%s() is missing argument '%s'", name, d->params[i]->chars);
            return NULL;
        }
    }
    for (int i = 0; i < argc; i++)
        if (!check_param(I, d, name, i, &I->stack[base + i], base + i)) return NULL;
    if (VM_DEPTH(I) >= LUBE_MAX_FRAMES || base + d->nslots + d->code.maxstack > LUBE_STACK_MAX) {
        lube_error(I, "stack overflow: too much recursion");
        return NULL;
    }
    return push_frame(I, fn, name, base, argc);
}

/* May a call of d, whose parameters from `first` on are plain (see FuncDecl.plain), start without enter()?
 * When it has every argument, those of Number parameters are Numbers, and there is room for its frame. */
static inline int plain_args(Interp *I, FuncDecl *d, int base, int argc, int first) {
    if (argc != d->nparams || VM_DEPTH(I) >= LUBE_MAX_FRAMES || base + d->nslots + d->code.maxstack > LUBE_STACK_MAX) return 0;
    for (int i = first; d->types && i < argc; i++)
        if (d->types[i].set && !IS_NUM(I->stack[base + i])) return 0;
    return 1;
}

/* The frame of a call to fn whose argc arguments, at stack[base...], are checked (or need no check). */
LUBE_INLINE Frame *push_frame(Interp *I, ObjFunction *fn, const char *name, int base, int argc) {
    FuncDecl *d = fn->decl;
    for (int i = base + argc; i < base + d->nslots; i++) I->stack[i] = NIL_VAL;
    I->sp = base + d->nslots;
    Frame *f = new_frame(I);
    frame_init(I, f, I->frame, fn, d, name, d->line, base, argc, base - 1, base + d->nslots);
    I->frame = f;
    return f;
}

/* Ends the running frame with result r (or ERR_VAL); returns the call's
 * result for the caller. A constructor's result is `this`. */
static Value leave(Interp *I, Value r) {
    Frame *f = I->frame;
    if (IS_ERR(r)) trace_frame(I, f);
    pop_pending(I, f->pending);
    /* copy what is needed: an `on` block run below may reuse this frame */
    int base = f->base, construct = f->construct, entry = f->entry, call_base = f->call_base, nloans = f->nloans;
    Loan *loans = f->loans;
#if VM_SLIM_CALL
    const VarRef *lent = f->lent;
#endif
    Value made = NIL_VAL;
    if (construct) {
        made = I->stack[base];
        I->stack[base] = NIL_VAL;
    }
    pop_to(I, f->block ? f->opbase : base + f->argc);  /* a block's slots are its function's */
    I->frame = f->prev;
    while (I->nfwatch && I->fwatch[I->nfwatch - 1].depth > VM_DEPTH(I))  /* its `on` blocks end with it */
        release(I->fwatch[--I->nfwatch].token);
    if (I->dbg) debug_left_frame(I);
    if (construct) {
        if (IS_ERR(r)) {
            release(made);
        } else {
            release(r);  /* whatever the constructor returned is not used */
            r = made;
        }
    }
    if (loans) r = return_loans(I, base, loans, nloans, r);
#if VM_SLIM_CALL
    if (lent) {  /* (as return_loans gives a loan back: nothing watches x, or the call would have had a Loan) */
        Value self = I->stack[base], *root = &SLOT(lent->index);
        I->stack[base] = NIL_VAL;
        release(*root);
        *root = self;
    }
#endif
    if (!entry) pop_to(I, call_base);
    return r;
}

/* Dog(args): checks the arguments against the constructor (this class's or
 * the nearest parent's) and makes the instance with every field at its
 * starting value. *ctor is NULL when there is no constructor to run. */
static Value new_instance(Interp *I, ObjClass *cls, int argc, ObjFunction **ctor) {
    ObjClass *owner = cls;
    while (owner && IS_NIL(owner->constructor)) owner = owner->parent;
    FuncDecl *d = owner ? AS_FUNCTION(owner->constructor)->decl : NULL;
    int max = d ? d->nparams - 1 : 0, min = max;
    while (d && min > 0 && d->has_default[min]) min--;
    if (argc < min || argc > max) return arity_error(I, cls->name->chars, min, max, argc);
    if (!stack_room(I, 1)) return lube_error(I, "stack overflow: too much recursion");
    ObjMap *inst = new_map();
    inst->klass = cls;
    map_fill(inst, cls->field_names, cls->field_defaults, (size_t)cls->nfields);
    *ctor = d ? AS_FUNCTION(owner->constructor) : NULL;
    return OBJ_VAL(inst);
}

/* Starts calling stack[ci] with the argc arguments above it. A function
 * (or a constructor) gets a frame: returns 1 and the VM goes on in it.
 * Anything else finishes here: returns 0 with the result (or ERR_VAL) in
 * *out and the callee and arguments popped. `loans` (lent with &) are
 * returned when the call ends, after an error too. */
static int start_call(Interp *I, int ci, int argc, Loan *loans, int nloans, int this_locked, Value *out) {
    Value callee = I->stack[ci], r;
    int base = ci + 1;
    if (IS_FUNCTION(callee) && AS_FUNCTION(callee)->decl->native && !this_locked &&
        call_native(I, AS_FUNCTION(callee)->decl, base, argc, &r)) {
        /* its machine code did it (with what was lent: given back below) */
    } else if (IS_FUNCTION(callee)) {
        Frame *f = enter(I, AS_FUNCTION(callee), base, argc);
        if (f) {
            f->this_locked = this_locked;
            f->call_base = ci;
            f->loans = loans;
            f->nloans = nloans;
            if (loans) FRAME_SLOW_RET(f);
            return 1;
        }
        r = ERR_VAL;
    } else if (IS_CLASS(callee) && AS_CLASS(callee)->foreign) {  /* Sound(...): a plugin's type */
        r = lube_foreign_construct(I, AS_CLASS(callee), argc, &I->stack[base]);
    } else if (IS_CLASS(callee)) {
        ObjFunction *ctor = NULL;
        r = new_instance(I, AS_CLASS(callee), argc, &ctor);
        if (!IS_ERR(r) && ctor) {  /* `this` goes in front of the arguments */
            memmove(&I->stack[base + 1], &I->stack[base], sizeof(Value) * (size_t)argc);
            I->stack[base] = r;
            I->sp++;
            FuncDecl *d = ctor->decl;  /* (plain parameters, all given: nothing for enter() to check) */
            Frame *f = d->plain_ctor && plain_args(I, d, base, argc + 1, 1)
                           ? push_frame(I, ctor, callee_name(OBJ_VAL(ctor)), base, argc + 1) : enter(I, ctor, base, argc + 1);
            if (f) {
                f->construct = 1;
                f->call_base = ci;
                FRAME_SLOW_RET(f);
                return 1;
            }
            r = ERR_VAL;  /* the instance is popped with the arguments */
        }
    } else {
        ObjNative *nf = AS_NATIVE(callee);
        if (argc < nf->min_args || (nf->max_args >= 0 && argc > nf->max_args))
            r = arity_error(I, nf->name, nf->min_args, nf->max_args, argc);
        else
            r = (I->calling = nf)->fn(I, argc, &I->stack[base]);
    }
    if (loans) r = return_loans(I, base, loans, nloans, r);
    pop_to(I, ci);
    *out = r;
    return 0;
}

/* x.name(args): what to call, and how x goes along (see eval rules in the
 * README): a method of x's class, lending x when it can change; a function
 * stored in x; or the function `name`, as name(x, args), lending x when that
 * function changes its first argument. Takes over obj and the place's keys.
 * Pushes the callee and the receiver slot; returns 0, or -1 after an error. */
static int push_call(Interp *I, Value callee, int mode, Value self, Place *place) {
    I->stack[I->sp++] = callee;
    I->stack[I->sp++] = self;
    PendingCall *p = new_pending(I);
    p->callee = I->sp - 2;
    p->has_slot = 1;
    p->mode = mode;
    if (place) move_place(&p->place, place);
    return 0;
}

/* A package's member called at md: found through md's cache when it still holds (the entry at the slot it
 * remembers must still be that global's), else as package_member finds it, and remembered. */
static Value package_callee(Interp *I, Value pkgv, MethodDesc *md) {
    ObjString *pkg = AS_PACKAGE(pkgv)->name;
    ObjMap *g = I->globals;
    int at = md->cache_gslot - 1;
    if (md->cache_pkg == pkg && at >= 0 && (size_t)at < g->used && g->entries[at].live &&
        g->entries[at].key.as.obj == (Obj *)md->cache_key)
        return retain(g->entries[at].value);
    Value v = package_member(I, pkgv, md->name);
    if (IS_ERR(v)) return v;
    ObjString *key = lube_global_key(I, pkg, md->name);
    Value *slot = map_find(g, OBJ_VAL(key));
    if (slot) {
        MapEntry *e = (MapEntry *)(void *)((char *)slot - offsetof(MapEntry, value));
        md->cache_pkg = pkg;
        md->cache_key = key;
        md->cache_gslot = (int)(e - g->entries) + 1;
    }
    return v;
}

LUBE_NOINLINE static int prep_method(Interp *I, MethodDesc *md, Value obj, Place *place, int have_place) {
    ObjString *name = md->name;
    if (IS_COMPONENT(obj)) {  /* c.view(), c.leave(), c.toggle() */
        if (have_place) place_free(place);
        Value callee = component_method(I, AS_COMPONENT(obj), name);
        if (!IS_ERR(callee)) return push_call(I, callee, SELF_VALUE, obj, NULL);
        release(obj);
        return -1;
    }
    if (IS_PACKAGE(obj)) {  /* geometry.area(s): a package's function */
        Value callee = package_callee(I, obj, md);
        release(obj);
        if (have_place) place_free(place);
        if (IS_ERR(callee)) return -1;
        if (!IS_CALLABLE(callee) && !IS_CLASS(callee)) {
            lube_error(I, "'%s' is not a function (it is %s)", name->chars, a_type(callee));
            release(callee);
            return -1;
        }
        return push_call(I, callee, SELF_NONE, NIL_VAL, NULL);
    }
    if (IS_INSTANCE(obj)) {
        ObjClass *klass = AS_MAP(obj)->klass;
        if (md->cache_class != klass || md->cache_epoch != I->epoch) {  /* (found the slow way once per class) */
            md->cache_class = klass;
            md->cache_method = find_method(klass, name);
            md->cache_epoch = I->epoch;
        }
        Value m = md->cache_method;
        if (!IS_NIL(m)) {
            if (have_place && place_changeable(I, place)) {
                release(obj);
                return push_call(I, retain(m), SELF_LEND, NIL_VAL, place);
            }
            if (have_place) place_free(place);
            return push_call(I, retain(m), SELF_LOCKED, obj, NULL);
        }
    }
    Value own;
    if (IS_MAP(obj) && map_get(AS_MAP(obj), OBJ_VAL(name), &own)) {
        Value callee = retain(own);
        release(obj);
        if (have_place) place_free(place);
        if (!IS_CALLABLE(callee)) {
            lube_error(I, "'%s' is not a function (it is %s)", name->chars, a_type(callee));
            release(callee);
            return -1;
        }
        return push_call(I, callee, SELF_NONE, NIL_VAL, NULL);
    }
    Value tm;
    if (IS_FOREIGN(obj) && map_get(AS_FOREIGN(obj)->klass->methods, OBJ_VAL(name), &tm)) {  /* sock.send(...) */
        if (have_place) place_free(place);
        return push_call(I, retain(tm), SELF_VALUE, obj, NULL);
    }
    /* bytes.pack(...): a method of its type (io.c) */
    if (IS_OBJ(obj) && LUBE_TM(OBJ_TYPE(obj)) && map_get(I->tmethods[OBJ_TYPE(obj) - OBJ_BYTES], OBJ_VAL(name), &tm)) {
        if (!AS_NATIVE(tm)->changes_first) {
            if (have_place) place_free(place);
            return push_call(I, retain(tm), SELF_VALUE, obj, NULL);
        }
        if (!have_place) {  /* (it changes it: lent, from its variable) */
            lube_error(I, "%s.%s() changes it, so call it on a variable (or a field or index of one)", type_name(obj),
                       name->chars);
            release(obj);
            return -1;
        }
        release(obj);
        return push_call(I, retain(tm), SELF_LEND, NIL_VAL, place);
    }
    const VarRef *r = &md->fallback;
    if (r->kind == VAR_GLOBAL && !global_exists(I, r)) {
        lube_error(I, "%s has no method '%s'", type_name(obj), name->chars);
        release(obj);
        if (have_place) place_free(place);
        return -1;
    }
    Value callee = get_var(I, r);
    if (!IS_ERR(callee) && !IS_CALLABLE(callee) && !IS_CLASS(callee)) {
        lube_error(I, "'%s' is not a function (it is %s)", name->chars, a_type(callee));
        release(callee);
        callee = ERR_VAL;
    }
    if (IS_ERR(callee)) {
        release(obj);
        if (have_place) place_free(place);
        return -1;
    }
    if (takes_first_by_ref(callee)) {
        release(obj);  /* drop our reference so the value can change in place */
        if (!have_place) {
            lube_error(I, "%s() changes its first argument, so call it on a variable (or a field or index of one)",
                       callee_name(callee));
            release(callee);
            return -1;
        }
        return push_call(I, callee, SELF_LEND, NIL_VAL, place);
    }
    if (have_place) place_free(place);
    return push_call(I, callee, SELF_VALUE, obj, NULL);
}

/* Moves a place's keys off the top of the stack. */
static void take_place(Interp *I, PlaceDesc *pd, Place *p) {
    int from = I->sp - pd->nkeys;
    p->root = &pd->var;
    p->nkeys = pd->nkeys;
    for (int j = 0; j < pd->nkeys; j++) {
        p->keys[j] = I->stack[from + j];
        p->is_field[j] = pd->is_field[j];
        I->stack[from + j] = NIL_VAL;
    }
    I->sp = from;
}

/* INVOKE: the arguments of the pending call are on the stack (values, and
 * the keys of & places). Lines them up after the callee, lends the places,
 * and starts the call. Returns as start_call does, or -1 after an error. */
LUBE_NOINLINE static int invoke(Interp *I, Code *code, CallDesc *cd, Value *out) {
    PendingCall *pc = &I->pending[--I->npending];  /* its place moves into the loans */
    int ci = pc->callee, mode = pc->mode, has_self = mode != SELF_NONE;
    int dst = ci + 1 + has_self, src = ci + 1 + pc->has_slot;
    int nloans = mode == SELF_LEND;
    for (int i = 0; i < cd->nargs; i++) nloans += cd->place[i] >= 0;
    Loan *loans = nloans ? new_loans(I, nloans) : NULL;
    int k = 0;
    if (mode == SELF_LEND) {
        loans[0].pos = 0;
        move_place(&loans[0].place, &pc->place);
        loans[0].holder = NIL_VAL;
        loans[0].lent = 0;
        k = 1;
    }
    /* a & argument's keys take its place: collect the values apart, then line them up */
    Value small[16], *vals = cd->nargs <= 16 ? small : xmalloc(sizeof(Value) * (size_t)cd->nargs);
    for (int i = 0; i < cd->nargs; i++) {
        if (cd->place[i] < 0) {
            vals[i] = I->stack[src];
            I->stack[src++] = NIL_VAL;
        } else {
            PlaceDesc *pd = &code->places[cd->place[i]];
            Loan *L = &loans[k++];
            L->pos = has_self + i;
            L->holder = NIL_VAL;
            L->lent = 0;
            L->place.root = &pd->var;
            L->place.nkeys = pd->nkeys;
            for (int j = 0; j < pd->nkeys; j++) {
                L->place.keys[j] = I->stack[src];
                L->place.is_field[j] = pd->is_field[j];
                I->stack[src++] = NIL_VAL;
            }
            vals[i] = NIL_VAL;  /* filled when the place is lent */
        }
    }
    for (int i = 0; i < cd->nargs; i++) I->stack[dst++] = vals[i];
    if (vals != small) free(vals);
    I->sp = dst;
    int argc = has_self + cd->nargs;
    Value callee = I->stack[ci];
    if (check_loans(I, callee, argc, loans, k, mode == SELF_LOCKED) < 0) goto fail;
    for (int j = 0; j < k; j++) {
        for (int m = 0; m < j; m++) {
            if (same_root(&loans[m].place, &loans[j].place)) {
                lube_error(I, "'%s' is passed with & more than once in this call", lube_shown(loans[j].place.root->name));
                goto fail;
            }
        }
        if (check_changeable(I, &loans[j].place) < 0) goto fail;
        const VarRef *root = loans[j].place.root;
        ObjString *watched = root->kind == VAR_GLOBAL ? watched_name(I, root->name) : NULL;
        const VarRef *lwatched = root->kind == VAR_LOCAL && root->watch && root->watch->n ? root : NULL;
        Value before = lwatched ? local_before(I, root->index, root->watch) : value_before(I, watched);
        Value v;
        if (place_take(I, &loans[j].place, &loans[j].holder, &v) < 0) {
            release(before);
            goto fail;
        }
        loans[j].lent = 1;
        loans[j].watched = watched;
        loans[j].lwatched = lwatched;
        loans[j].before = before;
        I->stack[ci + 1 + loans[j].pos] = v;
    }
    return start_call(I, ci, argc, loans, k, mode == SELF_LOCKED && IS_FUNCTION(callee), out);
fail:
    return_loans(I, ci + 1, loans, k, ERR_VAL);  /* gives back those already taken */
    return -1;
}

/* class Name extends Parent { ... }, first part: the class, with the
 * parent's fields. For a hot reload of a class that exists, the new
 * definition is built apart and moved into it at OP_CLASS_END. Pushes the
 * class it replaces (or nothing) and the new class. */
LUBE_NOINLINE static int class_begin(Interp *I, ClassDesc *cd) {
    ObjClass *into = NULL;
    if (cd->hot) {
        Value *old = map_find(I->globals, OBJ_VAL(cd->key));
        if (old && IS_CLASS(*old)) into = AS_CLASS(*old);
        else if (old) list_push(I->hot_old, retain(*old));  /* something else had the name */
    }
    ObjClass *parent = NULL;
    if (cd->parent.name) {
        Value p = get_var(I, &cd->parent);
        if (IS_ERR(p)) return -1;
        parent = IS_CLASS(p) ? AS_CLASS(p) : NULL;  /* classes live until shutdown */
        release(p);
        if (!parent) {
            lube_error(I, "'%s' is not a class, so %s cannot extend it", lube_shown(cd->parent.name), cd->name->chars);
            return -1;
        }
        for (ObjClass *a = parent; into && a; a = a->parent) {
            if (a == into) {
                lube_error(I, "%s cannot extend %s: %s already extends %s", into->name->chars, parent->name->chars,
                           parent->name->chars, into->name->chars);
                return -1;
            }
        }
    }
    int inherited = parent ? parent->nfields : 0, total = inherited + cd->nfields;
    ObjClass *c = (ObjClass *)alloc_object(sizeof(ObjClass), OBJ_CLASS);
    c->name = cd->name;
    c->key = cd->key;
    c->parent = parent;
    c->nfields = 0;
    c->field_names = xmalloc(sizeof(ObjString *) * (size_t)(total ? total : 1));
    c->field_defaults = xmalloc(sizeof(Value) * (size_t)(total ? total : 1));
    c->field_types = xmalloc(sizeof(TypeDesc) * (size_t)(total ? total : 1));
    c->methods = new_map();
    c->constructor = NIL_VAL;
    c->foreign = NULL;
    list_push(into ? I->hot_old : I->classes, OBJ_VAL(c));  /* keeps it alive until shutdown */
    for (int i = 0; i < inherited; i++, c->nfields++) {  /* the parent's fields come first */
        c->field_names[i] = parent->field_names[i];
        c->field_defaults[i] = retain(parent->field_defaults[i]);
        c->field_types[i] = parent->field_types[i];
    }
    I->stack[I->sp++] = into ? retain(OBJ_VAL(into)) : NIL_VAL;
    I->stack[I->sp++] = retain(OBJ_VAL(c));
    return 0;
}

static void class_end(Interp *I, ClassDesc *cd) {
    I->epoch++;  /* (a class updated by a hot reload has new methods) */
    ObjClass *c = AS_CLASS(I->stack[I->sp - 1]);
    Value into_val = I->stack[I->sp - 2];
    for (int i = 0; i < cd->nmethods; i++) {
        FuncDecl *d = cd->methods[i];
        d->owner = c;
        map_set(c->methods, retain(OBJ_VAL(d->name)), make_closure(I, d));
    }
    if (cd->ctor) {
        cd->ctor->owner = c;
        c->constructor = make_closure(I, cd->ctor);
    }
    if (IS_CLASS(into_val)) {  /* hot reload: swap the new definition in; c keeps the old one alive */
        ObjClass *into = AS_CLASS(into_val), old = *into;
        into->parent = c->parent;
        into->nfields = c->nfields;
        into->field_names = c->field_names;
        into->field_defaults = c->field_defaults;
        into->field_types = c->field_types;
        into->methods = c->methods;
        into->constructor = c->constructor;
        c->parent = old.parent;
        c->nfields = old.nfields;
        c->field_names = old.field_names;
        c->field_defaults = old.field_defaults;
        c->field_types = old.field_types;
        c->methods = old.methods;
        c->constructor = old.constructor;
        for (size_t i = 0; i < into->methods->used; i++) AS_FUNCTION(into->methods->entries[i].value)->decl->owner = into;
        if (!IS_NIL(into->constructor)) AS_FUNCTION(into->constructor)->decl->owner = into;
    } else {
        map_set(I->globals, retain(OBJ_VAL(cd->key)), retain(OBJ_VAL(c)));
        map_set(I->constants, retain(OBJ_VAL(cd->key)), NUM_VAL(ORIGIN_CLASS));
    }
    pop_to(I, I->sp - 2);
}

/* Can the `on` items be watched: is each global a var, and is each field
 * there? (A local was checked by the resolver.) */
static int check_items(Interp *I, OnDesc *od) {
    for (int i = 0; i < od->count; i++) {
        OnItem *it = &od->items[i];
        Value v;
        if (it->root.kind == VAR_GLOBAL) {
            ObjString *name = it->root.name;
            Origin origin;
            int failed;
            Value *g = lookup_global(I, name, &failed);
            if (!g) {
                if (!failed)
                    lube_error(I, "there is no global variable '%s' to watch (declare it with var first)", lube_shown(name));
                return -1;
            }
            if (global_origin(I, name, &origin)) {  /* a let, fn, class or built-in */
                char msg[256];
                unchangeable_message(msg, sizeof msg, lube_shown(name), origin);
                lube_error(I, "this 'on' block would never run: %s", msg);
                return -1;
            }
            v = *g;
        } else {
            v = SLOT(it->root.index);
        }
        for (int j = 0; j < it->npath && v.type != VAL_LENT; j++) {  /* (a global lent out: checked when it changes) */
            Value next;
            if (!IS_MAP(v) || !map_get(AS_MAP(v), OBJ_VAL(it->path[j]), &next)) {
                StrBuf sb;
                sb_init(&sb);
                sb_puts(&sb, it->name->chars);
                for (int k = 0; k <= j; k++) sb_printf(&sb, ".%s", it->path[k]->chars);
                lube_error(I, "cannot watch %s: %s has no field '%s'", sb.data, a_type(v), it->path[j]->chars);
                sb_free(&sb);
                return -1;
            }
            v = next;
        }
    }
    return 0;
}

/* on a, b (old, new) { ... } at the top level: registers the block. */
static int add_watcher(Interp *I, OnDesc *od) {
    if (check_items(I, od) < 0) return -1;
    if (I->nwatchers == I->cap_watchers) {
        I->cap_watchers = I->cap_watchers ? I->cap_watchers * 2 : 8;
        I->watchers = xrealloc(I->watchers, sizeof(Watcher) * (size_t)I->cap_watchers);
    }
    Watcher *w = &I->watchers[I->nwatchers++];
    w->items = od->items;
    w->nnames = od->count;
    w->fn = make_closure(I, od->func);
    w->file = lube_frame_src(I->frame)->path;
    w->running = 0;
    return 0;
}

static Value run_top(Interp *I, FuncDecl *d);
static int do_import(Interp *I, ObjString *rel);
static int import_package(Interp *I, ObjString *pkg, ObjString *bind);

/* The error for a global that is missing (g NULL) or lent out. */
static Value global_error(Interp *I, ObjString *name, Value *g) {
    if (!g) return lube_error(I, "undefined variable '%s'", lube_shown(name));
    return lube_error(I, IN_USE, lube_shown(name));
}

#define PUSH(v) (I->stack[I->sp++] = (v))
#define POP() (I->stack[--I->sp])
#define TOP (I->stack[I->sp - 1])
#define K(i) (code->consts[i])
#define SWITCH_FRAME()               \
    do {                             \
        f = I->frame;                \
        code = &f->proto->code;      \
        ip = f->pc;                  \
    } while (0)

/* What OP_CALL's plain call checks (see FuncDecl.fast_argc), worked out at d's first call. */
LUBE_NOINLINE static void decl_prepare(FuncDecl *d) {
    int unchecked = !d->types && !d->native && !d->ret.set && !d->strict;
    d->frame_need = d->nslots + d->code.maxstack;
    d->fast_argc = d->plain && unchecked ? d->nparams + 1 : -1;
    d->fast_margc = d->plain_method && unchecked ? d->nparams + 1 : -1;
}

/* A jump's target (see JUMP_ARG): pc0 is the instruction's own first word. */
#if VM_REL_JUMP
#define REL24(x) ((int)((x) ^ 0x800000) - 0x800000)
#define JUMP_TO(in) (pc0 + JUMP_REL(in))
#define JUMP_TO_WORD(w) (pc0 + REL24(w))  /* (w: an operand word that holds a JUMP_ARG) */
#else
#define JUMP_TO(in) (code->code + ARG_OF(in))
#define JUMP_TO_WORD(w) (code->code + (int)(w))
#endif

/* Runs one instruction, the one at I->frame->pc, the long way: everything it reads and leaves is the Interp's
 * (I->sp, I->frame, each frame's pc), so whatever it calls sees the VM as it is. Every instruction can be run
 * this way; execute() runs the common cases of the common ones itself, and comes here for the rest.
 *   VM_NEXT   done: I->frame->pc is the instruction to run next (in another frame, after a call or a return;
 *             at a catch, after an error it caught)
 *   VM_DONE   the frame execute() was entered for returned: its result is in I->ret_val
 *   VM_ERROR  an error left that frame */
enum { VM_NEXT, VM_DONE, VM_ERROR, VM_RAISED };

/* The instructions that cost most to send through vm_step, as functions of their own: vm_step calls them, and so
 * does execute()'s loop once it has written its state back (SYNC). Each works on the Interp's stack, as all of
 * the long way does. */

/* OP_CALL and OP_CALL_FN: the callee and its arguments are on the stack. 1: the call has a frame (I->frame; the
 * caller waits at its call); 0: it is done, and its result pushed; -1: an error is raised. */
LUBE_NOINLINE static int op_call(Interp *I, Frame *f, Code *code, Instr in) {
    Value r;
    int fn_call = OP_OF(in) == OP_CALL_FN, argc = fn_call ? ARG_OF(in) & 0xff : ARG_OF(in);
    int ci = I->sp - argc - 1;
    if (IS_FUNCTION(I->stack[ci])) {  /* a plain function: started here (see FuncDecl.plain) */
        ObjFunction *fn = AS_FUNCTION(I->stack[ci]);
        FuncDecl *d = fn->decl;
        int base = ci + 1;
        if (!d->fast_argc) decl_prepare(d);  /* (its first call: the next ones may start in execute's loop) */
        if (d->native && call_native(I, d, base, argc, &r)) {  /* its machine code did it */
            pop_to(I, ci);
            PUSH(r);
            return 0;
        }
        int ok = d->plain && argc == d->nparams && VM_DEPTH(I) < LUBE_MAX_FRAMES &&
                 base + d->nslots + d->code.maxstack <= LUBE_STACK_MAX;
        for (int i = 0; ok && d->types && i < argc; i++)
            ok = !d->types[i].set || IS_NUM(I->stack[base + i]);
        if (ok) {
            for (int i = base + argc; i < base + d->nslots; i++) I->stack[i] = NIL_VAL;
            I->sp = base + d->nslots;
            Frame *nf = new_frame(I);
            frame_init(I, nf, f, fn, d, d->name ? d->name->chars : "<lambda>", d->line, base, argc, ci,
                       base + d->nslots);
            I->frame = nf;
            return 1;
        }
    }
    if (fn_call && !IS_CALLABLE(I->stack[ci]) && !IS_CLASS(I->stack[ci])) {  /* replaced since */
        lube_error(I, "'%s' is not a function (it is %s)", AS_STRING(K((ARG_OF(in) >> 8) - 1))->chars,
                   a_type(I->stack[ci]));
        return -1;
    }
    if (takes_ref(I->stack[ci]) && check_loans(I, I->stack[ci], argc, NULL, 0, 0) < 0) return -1;
    if (start_call(I, ci, argc, NULL, 0, 0, &r)) {
        return 1;
    }
    if (IS_ERR(r)) return -1;
    PUSH(r);
    return 0;
}

/* OP_METHOD_PREP: x.name(, what to call and how x goes along (see prep_method). 0, or -1: an error is raised. */
LUBE_NOINLINE static int op_method_prep(Interp *I, Code *code, MethodDesc *md) {
    Place place;
    Value obj;
    int have_place = md->recv_place >= 0;
    if (have_place && code->places[md->recv_place].nkeys == 0) {
        /* x.method(...), x a variable holding an instance of the class
         * last seen here: the method, lending x (as prep_method does) */
        const VarRef *rv = &code->places[md->recv_place].var;
        int failed = 0;
        Value *root = rv->kind == VAR_LOCAL ? &SLOT(rv->index) : rv->kind == VAR_GLOBAL ? global_ref(I, rv, &failed) : NULL;
        if (failed) return -1;
        place.root = rv;
        place.nkeys = 0;
        if (root && IS_INSTANCE(*root) && AS_MAP(*root)->klass == md->cache_class && md->cache_epoch == I->epoch &&
            !IS_NIL(md->cache_method) && place_changeable(I, &place)) {
            push_call(I, retain(md->cache_method), SELF_LEND, NIL_VAL, &place);
            return 0;
        }
        if (root && IS_PACKAGE(*root) && md->cache_pkg == AS_PACKAGE(*root)->name) {
            /* math.sqrt(x): the package's function last called here, if its global is still where it was
             * (as package_callee finds it) */
            ObjMap *g = I->globals;
            int at = md->cache_gslot - 1;
            if (at >= 0 && (size_t)at < g->used && g->entries[at].live &&
                g->entries[at].key.as.obj == (Obj *)md->cache_key && IS_CALLABLE(g->entries[at].value)) {
                push_call(I, retain(g->entries[at].value), SELF_NONE, NIL_VAL, NULL);
                return 0;
            }
        }
        Value *fn = root && root->type == VAL_OBJ && !IS_MAP(*root) && !IS_PACKAGE(*root) && !LUBE_OWN_METHODS(OBJ_TYPE(*root)) &&
                    md->fallback.kind == VAR_GLOBAL
                        ? global_ref(I, &md->fallback, &failed) : NULL;
        if (failed) return -1;
        if (fn && IS_NATIVE(*fn)) {  /* xs.push(v), s.ends_with(t): a built-in, x first (as prep_method does) */
            if (AS_NATIVE(*fn)->changes_first) push_call(I, retain(*fn), SELF_LEND, NIL_VAL, &place);
            else push_call(I, retain(*fn), SELF_VALUE, retain(*root), NULL);
            return 0;
        }
    }
    if (have_place) {
        take_place(I, &code->places[md->recv_place], &place);
        obj = place_get(I, &place);
        if (IS_ERR(obj)) {
            place_free(&place);
            return -1;
        }
    } else {
        obj = POP();
    }
    if (prep_method(I, md, obj, &place, have_place) < 0) return -1;
    return 0;
}

/* OP_INVOKE: the pending call's arguments are on the stack. Returns as op_call does. */
LUBE_NOINLINE static int op_invoke(Interp *I, Frame *f, Code *code, CallDesc *cd) {
    Value r;
    PendingCall *pc = &I->pending[I->npending - 1];
    if (IS_FUNCTION(I->stack[pc->callee]) && !AS_FUNCTION(I->stack[pc->callee])->decl->fast_argc)
        decl_prepare(AS_FUNCTION(I->stack[pc->callee])->decl);  /* (its first call: the next may start in execute's loop) */
    if (pc->mode == SELF_LEND && pc->place.nkeys == 0 && !cd->refs && IS_FUNCTION(I->stack[pc->callee])) {
        /* x.method(args), x a variable: lend x straight into the new frame (see invoke) */
        ObjFunction *fn = AS_FUNCTION(I->stack[pc->callee]);
        FuncDecl *d = fn->decl;
        const VarRef *rv = pc->place.root;
        int ci = pc->callee, base = ci + 1, argc = 1 + cd->nargs, failed = 0;
        int ok = d->plain_method && argc == d->nparams && VM_DEPTH(I) < LUBE_MAX_FRAMES &&
                 base + d->nslots + d->code.maxstack <= LUBE_STACK_MAX &&
                 (rv->kind == VAR_LOCAL ? !(rv->watch && rv->watch->n)
                                        : !(I->nwatchers || I->nfwatch) || !watched_name(I, rv->name));
        for (int i = 1; ok && d->types && i < argc; i++) ok = !d->types[i].set || IS_NUM(I->stack[base + i]);
        Value *root = !ok ? NULL : rv->kind == VAR_LOCAL ? &SLOT(rv->index) : global_ref(I, rv, &failed);
        if (failed) return -1;
        if (root && root->type != VAL_LENT) {
            if (d->native) {  /* its machine code, x lent to it (and given back changed, when it ran) */
                I->stack[base] = *root;
                int ran = call_native(I, d, base, argc, &r);
                *root = I->stack[base];
                I->stack[base] = NIL_VAL;
                if (ran) {
                    I->npending--;
                    pop_to(I, ci);
                    PUSH(r);
                    return 0;
                }
            }
            Loan *L = new_loans(I, 1);
            L->pos = 0;
            move_place(&L->place, &pc->place);
            L->holder = NIL_VAL;
            L->lent = 1;
            L->watched = NULL;
            L->lwatched = NULL;
            L->before = NIL_VAL;
            I->npending--;
            I->stack[base] = *root;  /* its arguments are in place after it */
            *root = rv->kind == VAR_GLOBAL ? LENT_VAL : NIL_VAL;
            for (int i = base + argc; i < base + d->nslots; i++) I->stack[i] = NIL_VAL;
            I->sp = base + d->nslots;
            Frame *nf = new_frame(I);
            frame_init(I, nf, f, fn, d, d->name->chars, d->line, base, argc, ci, base + d->nslots);
            nf->loans = L;
            nf->nloans = 1;
            FRAME_SLOW_RET(nf);
            I->frame = nf;
            return 1;
        }
    }
    if (!cd->refs && IS_NATIVE(I->stack[pc->callee]) && pc->mode != SELF_LOCKED) {
        /* x.f(args), f a built-in (s.ends_with(t), xs.push(v)), or a package's (math.sqrt(x), with no x
         * in front): called here (see invoke) */
        ObjNative *nf = AS_NATIVE(I->stack[pc->callee]);
        int ci = pc->callee, self = pc->mode != SELF_NONE, argc = self + cd->nargs, failed = 0;
        int lend = pc->mode == SELF_LEND, first = ci + 1 + (self ? 0 : pc->has_slot);
        const VarRef *rv = lend ? pc->place.root : NULL;
        Value *root = !lend ? NULL : pc->place.nkeys || !nf->changes_first || !place_changeable(I, &pc->place) ? NULL
                    : rv->kind == VAR_LOCAL ? (rv->watch && rv->watch->n ? NULL : &SLOT(rv->index))
                    : (I->nwatchers || I->nfwatch) && watched_name(I, rv->name) ? NULL : global_ref(I, rv, &failed);
        if (failed) return -1;
        if (argc >= nf->min_args && (nf->max_args < 0 || argc <= nf->max_args) && (!lend || (root && root->type != VAL_LENT)) &&
            (lend || !nf->changes_first)) {
            I->npending--;
            if (lend) {  /* x is lent to it (in use meanwhile), and given back */
                I->stack[ci + 1] = *root;
                *root = rv->kind == VAR_GLOBAL ? LENT_VAL : NIL_VAL;
            }
            r = (I->calling = nf)->fn(I, argc, &I->stack[first]);
            if (lend) {
                root = rv->kind == VAR_LOCAL ? &SLOT(rv->index) : global_ref(I, rv, &failed);
                *root = I->stack[ci + 1];
                I->stack[ci + 1] = NIL_VAL;
            }
            pop_to(I, ci);
            if (IS_ERR(r)) return -1;
            PUSH(r);
            return 0;
        }
    }
    int started = invoke(I, code, cd, &r);
    if (started < 0) return -1;
    if (started) {
        return 1;
    }
    if (IS_ERR(r)) return -1;
    PUSH(r);
    return 0;
}

/* OP_RETURN and OP_RETURN_NIL: the running frame f returns r. VM_NEXT: its caller goes on (the result is pushed,
 * its pc is past the call); VM_DONE: it was the frame execute() was entered for (the result is in I->ret_val);
 * VM_RAISED: an error is raised (in f, or, once f is left, in its caller, still at its call). */
LUBE_NOINLINE static int op_return(Interp *I, Frame *f, Value r) {
    if (f->fn && (f->proto->ret.set || f->proto->strict)) {
        r = check_return(I, f->proto, lube_frame_name(f), r);
        if (IS_ERR(r)) return VM_RAISED;
    }
    if (!f->entry && !f->construct && !f->loans && !f->block && I->npending == f->pending && !I->dbg && !FRAME_LENT(f) &&
        !(I->nfwatch && I->fwatch[I->nfwatch - 1].depth >= VM_DEPTH(I))) {  /* nothing else to do (see leave) */
        pop_to(I, f->call_base);  /* its slots, the arguments and the callee */
        I->frame = f->prev;
        I->frame->pc++;  /* past the call it waited on */
        PUSH(r);
        return VM_NEXT;
    }
    if (f->nloans == 1 && !f->entry && !f->construct && !f->block && I->npending == f->pending && !I->dbg &&
        !(I->nfwatch && I->fwatch[I->nfwatch - 1].depth >= VM_DEPTH(I)) && f->loans->lent &&
        f->loans->place.nkeys == 0 && IS_NIL(f->loans->before)) {
        /* x.method(...) lent x, a variable, and nothing watches it: x gets it back (see leave) */
        Loan *L = f->loans;
        Value lent = I->stack[f->base + L->pos];
        I->stack[f->base + L->pos] = NIL_VAL;
        pop_to(I, f->call_base);
        I->frame = f->prev;
        const VarRef *rv = L->place.root;
        int failed;
        Value *root = rv->kind == VAR_LOCAL ? &SLOT(rv->index) : global_ref(I, rv, &failed);
        release(*root);
        *root = lent;
        free_loans(I, L, 1);
        I->frame->pc++;
        PUSH(r);
        return VM_NEXT;
    }
    {
        int entry = f->entry;
        r = leave(I, r);
        if (entry) {
            I->ret_val = r;  /* (execute takes it) */
            return VM_DONE;
        }
        if (IS_ERR(r)) return VM_RAISED;  /* an `on` block raised one */
        I->frame->pc++;  /* past the call it waited on */
        PUSH(r);
        return VM_NEXT;
    }
}

/* OP_ASSIGN, OP_ASSIGN_DROP, and what OP_SET_LOCAL leaves: place op= value (see AssignDesc), the place's keys and
 * the value on the stack. drop: a statement (the value is not kept). 0, or -1: an error is raised. */
LUBE_NOINLINE static int op_assign(Interp *I, Frame *f, Code *code, int assign_at, int drop) {
    Value r;
    AssignDesc *ad = &code->assigns[assign_at];
    PlaceDesc *pd = &code->places[ad->place];
    Value rhs = drop && ad->kconst >= 0 ? K(ad->kconst) : POP();
    if (pd->nkeys == 0 && pd->var.kind == VAR_LOCAL && pd->var.changeable && pd->var.origin != ORIGIN_THIS) {
        int at = f->base + pd->var.index;
        Value *slot = &I->stack[at];  /* a local: the common case */
        if (ad->op == T_ASSIGN) {
            r = I->decl[at].kind == TD_NUM && IS_NUM(rhs) ? rhs : keep_type(I, &pd->var, rhs);
        } else if (IS_NUM(*slot) && IS_NUM(rhs)) {  /* a Number var stays a Number */
            if (!num_op(compound_base(ad->op), slot->as.number, rhs.as.number, &r))
                r = binary_op(I, compound_base(ad->op), *slot, rhs);
        } else {
            goto assign_general;
        }
        if (IS_ERR(r)) return -1;
        Value old = *slot;
        *slot = r;
        release(old);
        if (!drop) PUSH(retain(r));
        return 0;
    }
    if (pd->nkeys == 0 && pd->var.kind == VAR_GLOBAL && (ad->op == T_ASSIGN || IS_NUM(rhs))) {
        Origin origin;
        int failed;
        Value *g = global_slot(I, code, ad->global, &failed);
        if (failed) return -1;
        if (!g || g->type == VAL_LENT || global_origin_ref(I, &pd->var, &origin) ||
            (ad->op != T_ASSIGN && !IS_NUM(*g)))
            goto assign_general;  /* errors and the rest: the general way */
        if (ad->op == T_ASSIGN)  /* (a var holding a plain Number is a Number, or untyped) */
            r = IS_NUM(*g) && IS_NUM(rhs) ? rhs : keep_type(I, &pd->var, rhs);
        else if (!IS_NUM(*g) || !num_op(compound_base(ad->op), g->as.number, rhs.as.number, &r))
            r = binary_op(I, compound_base(ad->op), *g, rhs);
        if (IS_ERR(r)) return -1;
        Value old = *g;
        *g = r;
        release(old);
        if (!drop) PUSH(retain(r));
        return 0;
    }
    if (pd->nkeys > 0 && fast_assign(I, pd, ad->op, &rhs)) {  /* xs[i] = v, p.x += 1, bodies[i].vx -= d */
        pop_to(I, I->sp - pd->nkeys);  /* the keys */
        if (!drop) PUSH(retain(rhs));
        return 0;
    }
assign_general: {
    Place p;
    take_place(I, pd, &p);
    r = do_assign(I, ad->op, &p, rhs);
    if (IS_ERR(r)) return -1;
    if (drop) release(r);
    else PUSH(r);
    return 0;
}
}

/* OP_INTERP: the n values on top of the stack, as one str (formatting runs no code of the program: one buffer, kept). */
LUBE_NOINLINE static void op_interp(Interp *I, int n) {
    StrBuf *sb = &I->interp;
    sb->len = 0;
    for (int i = I->sp - n; i < I->sp; i++) append_value(sb, I->stack[i], 0);
    pop_to(I, I->sp - n);
    PUSH(OBJ_VAL(new_string(sb->data ? sb->data : "", sb->len)));
    if (sb->cap > 65536) sb_free(sb);  /* (not a large one) */
}

/* An error is raised (I->error_val), in the newest frame, at its pc: finds a catch in that frame or the ones it
 * was called from, leaving those without one. VM_NEXT: caught (that frame's pc is the catch); VM_ERROR: the frame
 * execute() was entered for is left too. */
LUBE_NOINLINE static int vm_unwind(Interp *I) {
    for (;;) {
        Frame *f = I->frame;
        Code *code = &f->proto->code;
        int at = (int)(f->pc - code->code);
        Handler *h = NULL;
        for (int i = 0; i < code->nhandlers && !h; i++)  /* inner tries come first */
            if (at >= code->handlers[i].start && at < code->handlers[i].end) h = &code->handlers[i];
        if (h && !I->exiting) {  /* (exit(), in a host program: no catch stops it) */
            pop_pending(I, f->pending);
            pop_to(I, f->opbase + h->sp);
            clear_slots(I, h->clear_from, f->proto->nslots - h->clear_from);
            Value err = I->error_val;
            I->error_val = NIL_VAL;
            trace_reset(I);
            if (h->catch_slot >= 0) set_slot(I, h->catch_slot, err);
            else release(err);
            f->pc = code->code + h->target;
            return VM_NEXT;
        }
        int entry = f->entry;
        leave(I, ERR_VAL);
        if (entry) return VM_ERROR;
    }
}

LUBE_NOINLINE static int vm_step(Interp *I) {
    Frame *f = I->frame;
    Code *code = &f->proto->code;
    const Instr *ip = f->pc, *pc0;
    Value r;
    int assign_at = 0, assign_drop = 0;  /* (OP_SET_LOCAL goes on in OP_ASSIGN's code) */
    if (I->dbg) debug_hook(I);  /* the debugger looks before each instruction */
    Instr in = *ip;
    pc0 = ip++;
    (void)pc0;
    {
        switch ((OpCode)OP_OF(in)) {
        case OP_CONST:
            PUSH(retain(K(ARG_OF(in))));
            break;
        case OP_POP:
            release(POP());
            break;
        case OP_POPN:
            pop_to(I, I->sp - ARG_OF(in));
            break;
        case OP_LOCAL:
            PUSH(retain(I->stack[f->base + ARG_OF(in)]));
            break;
        case OP_CAPTURE:
            PUSH(retain(f->fn->captures[ARG_OF(in)]));
            break;
        case OP_SELF:
            PUSH(retain(OBJ_VAL(f->fn)));
            break;
        case OP_GLOBAL:
        case OP_GLOBAL_VALUE: {  /* for GLOBAL_VALUE, a class's own name alone makes an instance: Dog is Dog() */
            ObjString *name = AS_STRING(K(ARG_OF(in)));
            int failed;
            Value *g = global_slot(I, code, ARG_OF(in), &failed);
            if (!g || g->type == VAL_LENT) {
                if (!failed) global_error(I, name, g);
                goto error;
            }
            r = retain(*g);
            PUSH(r);
            if (OP_OF(in) == OP_GLOBAL) break;
            Origin origin;
            if (!IS_CLASS(r) || !global_origin(I, name, &origin) || origin != ORIGIN_CLASS) break;
            if (start_call(I, I->sp - 1, 0, NULL, 0, 0, &r)) {
                SWITCH_FRAME();
                break;
            }
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_DEF_LOCAL: {
            int slot = f->base + ARG_OF(in);
            Value v = POP();
            if (IS_NUM(v)) I->decl[slot].kind = TD_NUM;  /* the var keeps this type */
            else I->decl[slot] = desc_of(v);
            Value old = I->stack[slot];
            I->stack[slot] = v;
            release(old);
            break;
        }
        case OP_DEF_GLOBAL: {
            ObjString *name = AS_STRING(K(ARG_OF(in)));
            int kind = (int)*ip++;
            if (define_global(I, name, POP(), kind) < 0) goto error;
            break;
        }
        case OP_LAZY_GLOBAL: {  /* in a new isolate: made the first time it is used */
            Value init = POP(), entry;
            int kind = (int)*ip++;
            if (!I->lazy) I->lazy = new_map();
            ObjList *e = new_list();
            list_push(e, init);
            list_push(e, NUM_VAL(kind));
            entry = OBJ_VAL(e);
            map_set(I->lazy, retain(K(ARG_OF(in))), entry);
            break;
        }
        case OP_CLEAR:
            clear_slots(I, ARG_OF(in), (int)*ip++);
            break;
        case OP_NEED_VALUE:
            if (IS_NIL(TOP)) {  /* no value to use: a call to a function that returns nothing */
                I->sp--;
                if (ARG_OF(in))
                    lube_error(I, "%s() does not return a value, so there is nothing to use here",
                               AS_STRING(K(ARG_OF(in) - 1))->chars);
                else
                    lube_error(I, "this function call does not return a value, so there is nothing to use here");
                goto error;
            }
            break;
        case OP_INTERP:
            op_interp(I, ARG_OF(in));
            break;
        case OP_LIST:
        case OP_TYPED_LIST: {
            int n = ARG_OF(in);
            ObjList *l = new_list();
            if (OP_OF(in) == OP_TYPED_LIST) l->elem = (int)*ip++;
            list_reserve(l, (size_t)n);
            for (int i = I->sp - n; i < I->sp; i++) l->items[l->count++] = I->stack[i];
            I->sp -= n;
            PUSH(OBJ_VAL(l));
            break;
        }
        case OP_CHECK_ELEM: {
            Value v = TOP;
            r = lube_check_elem(I, list_type_name(ARG_OF(in)), ARG_OF(in), v);
            if (IS_ERR(r)) goto error;
            release(v);
            TOP = r;
            break;
        }
        case OP_MAP: {
            int n = ARG_OF(in);
            ObjMap *m = new_map();
            map_reserve(m, (size_t)n);
            for (int i = I->sp - 2 * n; i < I->sp; i += 2) map_set(m, I->stack[i], I->stack[i + 1]);
            I->sp -= 2 * n;
            PUSH(OBJ_VAL(m));
            break;
        }
        case OP_CHECK_KEY:
            if (!is_key(TOP)) {
                lube_error(I, "object keys must be str, Number or Bool values, not %s", a_type(TOP));
                goto error;
            }
            break;
        case OP_TYPED_MAP: {
            ObjMap *m = new_map();
            m->dict = 1;
            m->ktype = (unsigned char)(ARG_OF(in) & 0xff);
            m->vtype = (unsigned char)(ARG_OF(in) >> 8);
            PUSH(OBJ_VAL(m));
            break;
        }
        case OP_BINARY: {
            Value b = I->stack[I->sp - 1], a = I->stack[I->sp - 2];
            int op = ARG_OF(in);
            if (!IS_NUM(a) || !IS_NUM(b) || !num_op(op, a.as.number, b.as.number, &r)) {
                if (!str_eq_op(op, a, b, &r)) r = binary_op(I, (TokenType)op, a, b);
                release(a);
                release(b);
            }
            I->sp -= 2;
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_BINARY2: {  /* a op b, b (and perhaps a) read in place: a local, or a Number or str written out */
            int arg = ARG_OF(in), op = arg & 0xff, ka = (arg >> 8) & 3, kb = arg >> 10;
            Value b = kb == OPND_LOCAL ? I->stack[f->base + (int)ip[1]] : K(ip[1]);
            Value a = ka == OPND_LOCAL ? I->stack[f->base + (int)ip[0]] : ka == OPND_CONST ? K(ip[0]) : TOP;
            ip += 2;
            if (!IS_NUM(a) || !IS_NUM(b) || !num_op(op, a.as.number, b.as.number, &r)) {
                if (!str_eq_op(op, a, b, &r)) r = binary_op(I, (TokenType)op, a, b);
                if (ka == OPND_STACK) release(a);
            }
            if (ka == OPND_STACK) I->sp--;
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_CMP_JUMP: {  /* if a < b (or another comparison) is false: jump; a and b as in OP_BINARY2 */
            int word = (int)ip[0], op = word & 0xff, ka = (word >> 8) & 3, kb = word >> 10;
            int stacked = (ka == OPND_STACK) + (kb == OPND_STACK);
            Value b = kb == OPND_LOCAL ? I->stack[f->base + (int)ip[2]] : kb == OPND_CONST ? K(ip[2]) : TOP;
            Value a = ka == OPND_LOCAL   ? I->stack[f->base + (int)ip[1]]
                      : ka == OPND_CONST ? K(ip[1])
                                         : I->stack[I->sp - stacked];
            ip += 3;
            if (!IS_NUM(a) || !IS_NUM(b) || !num_op(op, a.as.number, b.as.number, &r)) {
                if (!str_eq_op(op, a, b, &r)) r = binary_op(I, (TokenType)op, a, b);
                if (ka == OPND_STACK) release(a);
                if (kb == OPND_STACK) release(b);
            }
            I->sp -= stacked;
            if (IS_ERR(r)) goto error;
            if (!IS_BOOL(r)) {  /* (a plugin's operator: an Array of answers, say) */
                lube_error(I, "a condition needs a Bool, and '%s' gave %s (use all() or any())", op == T_EQ ? "=="
                           : op == T_NEQ ? "!=" : op_symbol((TokenType)op), a_type(r));
                release(r);
                goto error;
            }
            if (!r.as.boolean) ip = JUMP_TO(in);
            break;
        }
        case OP_SET_LOCAL: {  /* x op= value, a statement, x a local var: the common Number cases here */
            int word = (int)ip[0], aop = word & 0xff, k = (word >> 8) - 1, at = f->base + ARG_OF(in);
            Value rhs = k >= 0 ? K(k) : TOP, *slot = &I->stack[at];
            if (IS_NUM(rhs) && (aop == T_ASSIGN ? I->decl[at].kind == TD_NUM  /* (so the value there is a Number too) */
                                                : IS_NUM(*slot) && num_op(compound_base(aop), slot->as.number,
                                                                          rhs.as.number, &rhs))) {
                *slot = rhs;
                if (k < 0) I->sp--;
                ip += 2;
                break;
            }
            assign_at = (int)ip[1];  /* the rest: the general way */
            assign_drop = 1;
            ip += 2;
            goto assign_with;
        }
        case OP_NEG: {
            Value v = POP();
            if (IS_INT(v)) r = int_negate(I, v);
            else if (IS_NUM(v)) r = NUM_VAL(-v.as.number);
            else if (IS_MONEY(v)) r = lube_money_negate(I, v);
            else if (!lube_foreign_neg(I, v, &r)) r = lube_error(I, "cannot negate %s", a_type(v));
            release(v);
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_NOT: {
            Value v = POP();
            int t = is_truthy(v);
            release(v);
            PUSH(BOOL_VAL(!t));
            break;
        }
        case OP_JUMP:
            ip = JUMP_TO(in);
            break;
        case OP_JUMP_IF_FALSE: {
            Value v = POP();
            int t = is_truthy(v);
            release(v);
            if (!t) ip = JUMP_TO(in);
            break;
        }
        case OP_AND:
        case OP_OR:
            if (is_truthy(TOP) == (OP_OF(in) == OP_OR)) ip = JUMP_TO(in);  /* keep the deciding value */
            else release(POP());
            break;
        case OP_LOOP:
            if (I->hot && ++I->hot_ticks >= 1024) hot_check(I);  /* a loop's turn is a safe point */
            ip = JUMP_TO(in);
            break;
        case OP_IS_TAG: {
            Value v = POP();
            int t = value_has_type(v, ARG_OF(in));
            release(v);
            PUSH(BOOL_VAL(t));
            break;
        }
        case OP_IS_CLASS: {
            Value c = POP(), v = POP();
            if (!IS_CLASS(c)) {
                lube_error(I, "'%s' is not a type or a class", AS_STRING(K(ARG_OF(in)))->chars);
                release(c);
                release(v);
                goto error;
            }
            int t = AS_CLASS(c)->foreign ? IS_FOREIGN(v) && AS_FOREIGN(v)->klass->foreign == AS_CLASS(c)->foreign
                                         : IS_INSTANCE(v) && is_subclass(AS_MAP(v)->klass, AS_CLASS(c));
            release(c);
            release(v);
            PUSH(BOOL_VAL(t));
            break;
        }
        case OP_RANGE_START:
            if (!IS_NUMERIC(TOP)) {
                lube_error(I, "range start must be a Number, not %s", a_type(TOP));
                goto error;
            }
            break;
        case OP_RANGE: {
            Value b = TOP, a = I->stack[I->sp - 2];
            if (!IS_NUMERIC(b)) {
                lube_error(I, "range end must be a Number, not %s", a_type(b));
                goto error;
            }
            I->sp -= 2;
            PUSH(OBJ_VAL(new_range(numeric_value(a), numeric_value(b), 1, ARG_OF(in))));
            break;
        }
        case OP_INDEX: {
            Value idx = POP(), obj = POP();
            if (IS_LIST(obj) && IS_NUM(idx)) {  /* the common case: a whole number in range */
                double d = idx.as.number;
                ObjList *l = AS_LIST(obj);
                if (d >= 0 && d < (double)l->count && d == (double)(size_t)d) {
                    r = retain(l->items[(size_t)d]);
                    release(obj);
                    PUSH(r);
                    break;
                }
            }
            r = index_get(I, obj, idx);
            release(obj);
            release(idx);
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_INDEX2: {  /* obj[index], index (and perhaps obj) read in place: see OP_BINARY2 */
            int ka = ARG_OF(in) & 3, kb = ARG_OF(in) >> 2;
            Value idx = kb == OPND_LOCAL ? I->stack[f->base + (int)ip[1]] : K(ip[1]);
            Value obj = ka == OPND_LOCAL ? I->stack[f->base + (int)ip[0]] : ka == OPND_CONST ? K(ip[0]) : TOP;
            ip += 2;
            double d = IS_NUM(idx) ? idx.as.number : -1;
            if (IS_LIST(obj) && d >= 0 && d < (double)AS_LIST(obj)->count && d == (double)(size_t)d)
                r = retain(AS_LIST(obj)->items[(size_t)d]);
            else
                r = index_get(I, obj, idx);
            if (ka == OPND_STACK) {
                release(obj);
                I->sp--;
            }
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_FIELD_LOCAL: {  /* x.name, x a local: read in place */
            Value obj = I->stack[f->base + ARG_OF(in)];
            ObjString *name = AS_STRING(K(*ip++));
            int fs = IS_INSTANCE(obj) ? field_slot(AS_MAP(obj), name) : -1;
            r = fs >= 0 ? retain(AS_MAP(obj)->entries[fs].value) : field_get(I, obj, name);
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_FIELD: {
            Value obj = POP();
            int fs = IS_INSTANCE(obj) ? field_slot(AS_MAP(obj), AS_STRING(K(ARG_OF(in)))) : -1;
            if (fs >= 0) {  /* a field of an instance: no hashing */
                r = retain(AS_MAP(obj)->entries[fs].value);
                release(obj);
                PUSH(r);
                break;
            }
            r = field_get(I, obj, AS_STRING(K(ARG_OF(in))));
            release(obj);
            if (IS_ERR(r)) goto error;
            PUSH(r);
            break;
        }
        case OP_CLOSURE:
            PUSH(make_closure(I, code->protos[ARG_OF(in)]));
            break;
        case OP_CHECK_CALLABLE:
            if (!IS_CALLABLE(TOP) && !IS_CLASS(TOP)) {
                if (ARG_OF(in))
                    lube_error(I, "'%s' is not a function (it is %s)", AS_STRING(K(ARG_OF(in) - 1))->chars, a_type(TOP));
                else
                    lube_error(I, "cannot call %s", a_type(TOP));
                goto error;
            }
            break;
        case OP_CALL:
        case OP_CALL_FN: {  /* FN: a top-level fn (argc | name constant + 1 << 8), not checked yet */
            int st = op_call(I, f, code, in);
            if (st < 0) goto error;
            if (st) SWITCH_FRAME();
            break;
        }
        case OP_CALL_PREP: {
            PendingCall *p = new_pending(I);
            p->callee = I->sp - 1;
            p->has_slot = 0;
            p->mode = SELF_NONE;
            break;
        }
        case OP_METHOD_PREP:
            if (op_method_prep(I, code, &code->methods[ARG_OF(in)]) < 0) goto error;
            break;
        case OP_PUSH_CHECK: {  /* xs.push(v), push(&xs, v): may OP_PUSH_DO append v? Else on to the call, at ip[0] */
            MethodDesc *md = &code->methods[ARG_OF(in)];
            const VarRef *rv = &code->places[md->recv_place].var;
            Value *root = &I->stack[f->base + rv->index], *fn;
            int failed = 0;
            if (((IS_LIST(*root) && AS_LIST(*root)->elem == TT_ANY) || IS_BYTES(*root)) && !(rv->watch && rv->watch->n) &&
                (fn = global_ref(I, &md->fallback, &failed)) != NULL && IS_NATIVE(*fn) && AS_NATIVE(*fn)->changes_first == 2)
                ip++;
            else if (failed) goto error;
            else ip = JUMP_TO_WORD(ip[0]);
            break;
        }
        case OP_PUSH_DO: {  /* ...appended as n_push does it (to a copy, if the List is shared: xs.push(xs)) */
            Value *root = &I->stack[f->base + ARG_OF(in)], v = POP();
            if (IS_LIST(*root)) {
                make_unique(root);
                list_push(AS_LIST(*root), v);  /* (the List takes it over) */
            } else {  /* Bytes: a byte, text or Bytes, or an error */
                Value two[2];
                two[0] = *root;
                two[1] = v;
                r = lube_bytes_push(I, 2, two);
                *root = two[0];
                release(v);
                if (IS_ERR(r)) goto error;
            }
            ip = JUMP_TO_WORD(ip[0]);
            if (OP_OF(*ip) == OP_POP) ip++;  /* (a statement: its nothing would be dropped) */
            else PUSH(NIL_VAL);
            break;
        }
        case OP_SUPER_PREP: {  /* the parent class's method (or constructor), with the same `this` */
            SuperDesc *sd = &code->supers[ARG_OF(in)];
            int skip = (int)*ip++;
            ObjClass *parent = f->fn->decl->owner->parent;
            Value callee = NIL_VAL;
            if (sd->name) {
                callee = parent ? find_method(parent, sd->name) : NIL_VAL;
                if (IS_NIL(callee)) {
                    lube_error(I, "%s has no method '%s' to call with super", parent ? parent->name->chars : "Object",
                               sd->name->chars);
                    goto error;
                }
            } else {
                for (ObjClass *c = parent; c && IS_NIL(callee); c = c->parent) callee = c->constructor;
                if (IS_NIL(callee)) {
                    if (sd->nargs == 0) {  /* nothing to run */
                        PUSH(NIL_VAL);
                        ip = code->code + skip;
                        break;
                    }
                    lube_error(I, "the parent class has no constructor to pass these arguments to");
                    goto error;
                }
            }
            Place place;
            place.root = &sd->self;
            place.nkeys = 0;
            if (place_changeable(I, &place)) push_call(I, retain(callee), SELF_LEND, NIL_VAL, &place);
            else push_call(I, retain(callee), SELF_LOCKED, retain(I->stack[f->base]), NULL);
            break;
        }
        case OP_INVOKE: {
            int st = op_invoke(I, f, code, &code->calls[ARG_OF(in)]);
            if (st < 0) goto error;
            if (st) SWITCH_FRAME();
            break;
        }
        case OP_RETURN:
            r = POP();
            goto do_return;
        case OP_RETURN_NIL:
            r = NIL_VAL;
        do_return: {
            int st = op_return(I, f, r);
            if (st == VM_RAISED) goto error;
            return st;  /* (the frame that goes on has its pc) */
        }
        case OP_THROW:
            lube_throw(I, POP());
            goto error;
        case OP_FOR_PREP: {  /* the loop owns what it iterates: changes to the variable go to a copy */
            Value it = TOP;
            double n = 0;
            if (IS_FOREIGN(it) && lube_foreign_iterable(it)) {  /* a plugin's object: x[0], x[1]... */
                if (lube_foreign_len(I, it, &n) < 0) goto error;
            } else if (!IS_RANGE(it) && !IS_LIST(it) && !IS_TEXT(it) && !IS_MAP(it) && !IS_CHANNEL(it) && !IS_BYTES(it)) {
                lube_error(I, "cannot loop over %s (expected a List, Range, str, Object, Channel or Bytes)", a_type(it));
                goto error;
            }
            PUSH(NUM_VAL(IS_RANGE(it) ? range_length(AS_RANGE(it)) : n));  /* position (in text: bytes; in a map:
                                                                  entries; a range or plugin's object: its length) */
            PUSH(NUM_VAL(0));  /* count */
            break;
        }
        case OP_FOR_NEXT: {
            if (IS_RANGE(I->stack[I->sp - 3]) && (int)ip[1] < 0) {  /* for i in a..b: the common case */
                double k = I->stack[I->sp - 1].as.number;
                if (k >= I->stack[I->sp - 2].as.number) {  /* (its length, from OP_FOR_PREP) */
                    ip = JUMP_TO(in);
                    break;
                }
                ObjRange *rg = AS_RANGE(I->stack[I->sp - 3]);
                Value *slot = &I->stack[f->base + (int)ip[0]], old = *slot;
                *slot = NUM_VAL(rg->start + k * rg->step);
                release(old);
                I->stack[I->sp - 1] = NUM_VAL(k + 1);
                ip += 2;
                break;
            }
            if (IS_LIST(I->stack[I->sp - 3]) && (int)ip[1] < 0) {  /* for x in xs: as common */
                ObjList *l = AS_LIST(I->stack[I->sp - 3]);
                double k = I->stack[I->sp - 1].as.number;
                if (k >= (double)l->count) {
                    ip = JUMP_TO(in);
                    break;
                }
                Value *slot = &I->stack[f->base + (int)ip[0]], old = *slot;
                *slot = retain(l->items[(size_t)k]);
                release(old);
                I->stack[I->sp - 1] = NUM_VAL(k + 1);
                ip += 2;
                break;
            }
            int s1 = f->base + (int)ip[0], s2 = (int)ip[1];
            int two = s2 >= 0;
            if (two) s2 += f->base;
            Value it = I->stack[I->sp - 3];
            double pos = I->stack[I->sp - 2].as.number, k = I->stack[I->sp - 1].as.number;
            Value first = NIL_VAL, second = NIL_VAL;
            int done = 0;
            if (IS_RANGE(it)) {
                ObjRange *rg = AS_RANGE(it);
                if (k >= pos) done = 1;  /* (its length, from OP_FOR_PREP) */
                else second = NUM_VAL(rg->start + k * rg->step);
            } else if (IS_CHANNEL(it)) {  /* waits for each value, until the channel is closed */
                int got = channel_next(I, it, &second);
                if (got < 0) goto error;
                done = !got;
            } else if (IS_LIST(it)) {
                ObjList *l = AS_LIST(it);
                if (k >= (double)l->count) done = 1;
                else second = retain(l->items[(size_t)k]);
            } else if (IS_TEXT(it)) {  /* by UTF-8 character */
                ObjString *s = text_of(it);
                size_t i = (size_t)pos;
                if (i >= s->length) {
                    done = 1;
                } else {
                    size_t len = utf8_seq_len((unsigned char)s->chars[i]);
                    if (i + len > s->length) len = s->length - i;
                    second = len == 1 ? retain(OBJ_VAL(I->chars[(unsigned char)s->chars[i]]))
                                      : OBJ_VAL(new_string(s->chars + i, len));
                    pos = (double)(i + len);
                }
            } else if (IS_BYTES(it)) {
                ObjBytes *b = AS_BYTES(it);
                if (k >= (double)b->len) done = 1;
                else second = NUM_VAL(b->data[(size_t)k]);
            } else if (IS_FOREIGN(it)) {
                if (k >= pos) {
                    done = 1;
                } else {
                    lube_foreign_index(I, it, NUM_VAL(k), &second);
                    if (IS_ERR(second)) goto error;
                }
            } else {  /* a map or object: keys (and values) */
                ObjMap *m = AS_MAP(it);
                size_t i = (size_t)pos;
                while (i < m->used && !m->entries[i].live) i++;
                if (i >= m->used) {
                    done = 1;
                } else {
                    first = retain(m->entries[i].key);
                    second = two ? retain(m->entries[i].value) : NIL_VAL;
                    pos = (double)(i + 1);
                }
            }
            if (done) {
                ip = JUMP_TO(in);
                break;
            }
            if (!IS_MAP(it)) first = NUM_VAL(k);
            if (IS_MAP(it) || two) {
                Value old = I->stack[s1];
                I->stack[s1] = first;
                release(old);
            }
            if (!IS_MAP(it) || two) {
                int s = two ? s2 : s1;
                Value old = I->stack[s];
                I->stack[s] = second;
                release(old);
            }
            I->stack[I->sp - 2] = NUM_VAL(pos);
            I->stack[I->sp - 1] = NUM_VAL(k + 1);
            ip += 2;
            break;
        }
        case OP_WATCH_BEFORE:  /* (nothing to compare with when no `on` block watches anything) */
            PUSH(I->nwatchers || I->nfwatch ? value_before(I, watched_name(I, AS_STRING(K(ARG_OF(in))))) : NIL_VAL);
            break;
        case OP_WATCH_AFTER: {
            r = POP();
            Value before = POP();
            if (!IS_NIL(before)) {
                r = after_change(I, AS_STRING(K(ARG_OF(in))), before, r);
                release(before);
                if (IS_ERR(r)) goto error;
            }
            PUSH(r);
            break;
        }
        case OP_ASSIGN:
        case OP_ASSIGN_DROP:  /* DROP: a statement (the value is not kept) */
            assign_drop = OP_OF(in) == OP_ASSIGN_DROP;
            assign_at = ARG_OF(in);
        assign_with:  /* (OP_SET_LOCAL goes on here) */
            if (op_assign(I, f, code, assign_at, assign_drop) < 0) goto error;
            break;
        case OP_IMPORT:
            if (do_import(I, AS_STRING(K(ARG_OF(in)))) < 0) goto error;
            break;
        case OP_IMPORT_PACKAGE: {
            int bind = (int)*ip++;
            if (import_package(I, AS_STRING(K(ARG_OF(in))), bind ? AS_STRING(K(bind - 1)) : NULL) < 0) goto error;
            break;
        }
        case OP_CLASS_BEGIN:
            if (class_begin(I, &code->classes[ARG_OF(in)]) < 0) goto error;
            break;
        case OP_CLASS_FIELD: {  /* before its starting value runs: a field the parent has already */
            ObjString *name = code->classes[ARG_OF(in)].fields[*ip++];
            ObjClass *c = AS_CLASS(TOP);
            int inherited = c->parent ? c->parent->nfields : 0;
            for (int j = 0; j < inherited; j++) {
                if (c->field_names[j] == name) {
                    lube_error(I, "field '%s' is already declared in %s", name->chars, c->parent->name->chars);
                    goto error;
                }
            }
            break;
        }
        case OP_CLASS_FIELD_SET: {  /* each instance starts with a copy of this value */
            ObjString *name = code->classes[ARG_OF(in)].fields[*ip++];
            Value v = POP();
            ObjClass *c = AS_CLASS(TOP);
            c->field_names[c->nfields] = name;
            c->field_defaults[c->nfields] = v;
            c->field_types[c->nfields] = desc_of(v);
            c->nfields++;
            break;
        }
        case OP_CLASS_END:
            class_end(I, &code->classes[ARG_OF(in)]);
            break;
        case OP_ON: {
            OnDesc *od = code->ons[ARG_OF(in)];
            if ((od->frame ? add_frame_watcher(I, od) : add_watcher(I, od)) < 0) goto error;
            break;
        }
        case OP_COMPONENT_BEGIN: {  /* fn ... component: the component, in its hidden slot while its body runs */
            ObjComponent *c = new_component(f->fn->decl);
            set_slot(I, ARG_OF(in), OBJ_VAL(c));
            c->depth = VM_DEPTH(I);
            break;
        }
        case OP_HANDLER: {  /* fn toggle() { ... } in a component's body: a function that runs it on the component */
            OnDesc *od = code->ons[ARG_OF(in)];
            ObjComponent *c = AS_COMPONENT(I->stack[f->base + f->fn->decl->comp_slot]);
            PUSH(bound_native(c, od->func->name->chars, n_handler, od, od->func->nparams));
            break;
        }
        case OP_COMPONENT_END: {  /* its body is done: its frame goes into the component, which is what it returns */
            ObjComponent *c = AS_COMPONENT(I->stack[f->base + ARG_OF(in)]);
            r = retain(OBJ_VAL(c));
            component_keep(I, c, f->base);
            goto do_return;
        }
        case OP_LWATCH_BEFORE: {
            LocalWatch *lw = &code->lwatch[ARG_OF(in)];
            PUSH(local_before(I, lw->slot, lw->list));
            break;
        }
        case OP_LWATCH_AFTER: {
            LocalWatch *lw = &code->lwatch[ARG_OF(in)];
            r = POP();
            Value before = POP();
            if (!IS_NIL(before)) {
                r = after_local(I, lw->slot, lw->list, before, r);
                release(before);
                if (IS_ERR(r)) goto error;
            }
            PUSH(r);
            break;
        }
        case OP_ARG_DEFAULT: {  /* jumps over a default's code when the argument was given */
            int skip = (int)*ip++;
            if (f->argc > ARG_OF(in)) ip = code->code + skip;
            break;
        }
        case OP_DEF_PARAM: {
            int i = ARG_OF(in);
            Value v = POP();
            if (!check_param(I, f->proto, lube_frame_name(f), i, &v, f->base + i)) {
                release(v);
                goto error;
            }
            Value old = I->stack[f->base + i];
            I->stack[f->base + i] = v;
            release(old);
            break;
        }
        case OP_PRINT_REPL:
            if (!IS_NIL(TOP)) {
                ObjString *text = value_repr(TOP);
                printf("%s\n", text->chars);
                release(OBJ_VAL(text));
            }
            break;
        case OP_HOT_KEEP: {  /* a function being replaced: a call to it may still be running */
            Value *old = map_find(I->globals, K(ARG_OF(in)));
            if (old) list_push(I->hot_old, retain(*old));
            break;
        }
        case OP_IF_GLOBAL: {  /* hot reload: a variable that exists keeps its value */
            int target = (int)*ip++;
            if (map_find(I->globals, K(ARG_OF(in)))) ip = code->code + target;
            break;
        }
        case OP_FAIL:
            lube_error(I, "%s", AS_STRING(K(ARG_OF(in)))->chars);
            goto error;
        case OP_NATIVE_LOOP: {  /* its machine code ran the loop (else, when it gave up, the bytecode runs it) */
            int skip = (int)*ip++;
            double unused;
            int left = LUBE_MAX_FRAMES - VM_DEPTH(I) < NATIVE_DEPTH ? LUBE_MAX_FRAMES - VM_DEPTH(I) : NATIVE_DEPTH;
            if (!I->dbg && !lube_natives[ARG_OF(in)].fnv(I, NULL, &unused, left)) ip = code->code + skip;
            break;
        }
        default:  /* LCOV_EXCL_START: the compiler emits only the opcodes above */
            lube_error(I, "internal error: bad instruction");
            goto error;
            /* LCOV_EXCL_STOP */
        }
        f->pc = ip;
        return VM_NEXT;

    error:  /* find a catch in this frame or the ones it was called from */
        return vm_unwind(I);
    }
}
#undef PUSH
#undef POP
#undef TOP
#undef K
#undef SWITCH_FRAME
#undef JUMP_TO
#undef JUMP_TO_WORD

/* ----------------------------------------------------------------------
 * execute(): the loop, in two tiers (see the VM_ switches in lube.h).
 *
 * vm_step() above runs any instruction, with the VM's state in the Interp.
 * The loop here keeps that state in C locals instead: ip (the next code
 * word), sp (the stack's top), base (the frame's first slot), consts and
 * the frame f. It runs the common cases of the common instructions with
 * those alone, and nothing here raises an error, runs the program's code
 * through C or looks at the Interp's stack: an instruction that would is
 * left untouched (`goto slow`), the locals are written back (SYNC), and
 * vm_step runs it. So the Interp's own I->sp, I->frame and f->pc are stale
 * while the loop runs, and right whenever anything can see them.
 *
 * An instruction here checks first and changes after: once it has popped,
 * pushed, released or moved ip it must finish. While it runs, ip is its own
 * first word (its other words are ip[1]...): it ends with NEXT(its length),
 * or, after a jump, NEXT_AT().
 *
 * Between the two: an instruction too common to send through vm_step, but
 * with work for the Interp's own stack or that can fail (a call the long
 * way, an assignment into a container, indexing an Object), says SYNC(),
 * calls what vm_step would (op_call, op_assign, index_get...), and reads
 * the stack's top back (LOAD_SP) or the whole frame (LOAD_FRAME); when what
 * it called raised an error, it goes to `raise` with the Interp's stack
 * still holding what the instruction had not let go of.
 * -------------------------------------------------------------------- */
#if VM_REGS
#define PUSH(v) (*sp++ = (v))
#define POP() (*--sp)
#define PEEK(n) (sp[-(n)])  /* PEEK(1) is the top */
#define DROP(n) (sp -= (n))
#define SP sp
#define SET_SP(p) (sp = (p))
#define BASE base
#define K(i) (consts[i])
#define SET_FRAME(nf) (f = (nf))
#define SYNC_REGS() (I->frame = f, I->sp = (int)(sp - I->stack))
#define LOAD_SP() (sp = I->stack + I->sp, CHECK_SYNCED(0))
#define LOAD_REGS() (base = f->slots, consts = f->proto->code.consts, sp = I->stack + I->sp)
#define CALL_REGS(d, slots_) (base = (slots_), consts = (d)->code.consts)
#else
#define PUSH(v) (I->stack[I->sp++] = (v))
#define POP() (I->stack[--I->sp])
#define PEEK(n) (I->stack[I->sp - (n)])
#define DROP(n) (I->sp -= (n))
#define SP (I->stack + I->sp)
#define SET_SP(p) (I->sp = (int)((p) - I->stack))
#define BASE (I->stack + f->base)
#define K(i) (f->proto->code.consts[i])
#define SET_FRAME(nf) (I->frame = f = (nf))
#define SYNC_REGS() ((void)0)
#define LOAD_SP() ((void)0)
#define LOAD_REGS() ((void)0)
#define CALL_REGS(d, slots_) ((void)0)
#endif
#define TOP PEEK(1)
#define LOCAL(i) (BASE[i])
#define CODE (&f->proto->code)

#if VM_LAZY_PC
#define SAVE_PC() (f->pc = ip)
#define STEP_PC() ((void)0)
#else
#define SAVE_PC() ((void)0)
#define STEP_PC() (f->pc = ip)  /* every instruction */
#endif
#ifdef VM_PC_CHECK
#define CHECK_SYNCED(yes) (vm_synced = (yes))
#else
#define CHECK_SYNCED(yes) ((void)0)
#endif
#define SYNC() (SAVE_PC(), SYNC_REGS(), CHECK_SYNCED(1))
#define LOAD_FRAME()                 \
    do {                             \
        f = I->frame;                \
        ip = f->pc;                  \
        LOAD_REGS();                 \
        CHECK_SYNCED(0);             \
    } while (0)

/* A jump (see JUMP_ARG): ip becomes its target. */
#if VM_REL_JUMP
#define REL24(x) ((int)((x) ^ 0x800000) - 0x800000)
#define GOTO_ARG(in) (ip += JUMP_REL(in))
#define GOTO_WORD(w) (ip += REL24(w))  /* (w: an operand word that holds a JUMP_ARG) */
#else
#define GOTO_ARG(in) (ip = CODE->code + ARG_OF(in))
#define GOTO_WORD(w) (ip = CODE->code + (int)(w))
#endif

#if VM_DBG_FOLD
#define DBG_CHECK() ((void)0)  /* (while the debugger watches, the loop runs nothing itself) */
#else
#define DBG_CHECK() do { if (I->dbg) goto slow; } while (0)
#endif
#ifdef VM_STATS  /* a counting build: which instructions go the long way, and how many run here */
static unsigned long vm_slow_count[256], vm_fast_count;
static void vm_stats_print(void) {
    static const char *const names[] = {
#define LUBE_OP_NAME(op) #op,
        LUBE_OPCODES(LUBE_OP_NAME)
#undef LUBE_OP_NAME
    };
    unsigned long slow = 0;
    for (int i = 0; i < OP_COUNT; i++) slow += vm_slow_count[i];
    fprintf(stderr, "vm: %lu instructions in the loop, %lu the long way (%.1f%%)\n", vm_fast_count - slow, slow,
            100.0 * (double)slow / (double)(vm_fast_count ? vm_fast_count : 1));
    for (int i = 0; i < OP_COUNT; i++)
        if (vm_slow_count[i]) fprintf(stderr, "  %-18s %lu\n", names[i], vm_slow_count[i]);
}
#define COUNT_FAST() (vm_fast_count++)
#define COUNT_SLOW() (vm_slow_count[OP_OF(*ip)]++)
#else
#define COUNT_FAST() ((void)0)
#define COUNT_SLOW() ((void)0)
#endif

/* The instructions the loop runs itself (the others always go the long way): interp_ops.h has their code. */
#define VM_FAST_OPS(X)                                                                                               \
    X(OP_CONST) X(OP_POP) X(OP_POPN) X(OP_LOCAL) X(OP_CAPTURE) X(OP_SELF) X(OP_GLOBAL) X(OP_GLOBAL_VALUE)            \
    X(OP_DEF_LOCAL) X(OP_CLEAR) X(OP_NEED_VALUE) X(OP_LIST) X(OP_MAP) X(OP_CHECK_KEY) X(OP_BINARY) X(OP_BINARY2)     \
    X(OP_CMP_JUMP) X(OP_SET_LOCAL) X(OP_NEG) X(OP_NOT) X(OP_JUMP) X(OP_JUMP_IF_FALSE) X(OP_AND) X(OP_OR) X(OP_LOOP)  \
    X(OP_INDEX) X(OP_INDEX2) X(OP_FIELD_LOCAL) X(OP_FIELD) X(OP_CLOSURE) X(OP_CHECK_CALLABLE) X(OP_CALL)             \
    X(OP_CALL_FN) X(OP_RETURN) X(OP_RETURN_NIL) X(OP_FOR_PREP) X(OP_FOR_NEXT) X(OP_WATCH_BEFORE) X(OP_WATCH_AFTER)   \
    X(OP_ARG_DEFAULT) X(OP_PUSH_CHECK) X(OP_PUSH_DO) X(OP_INTERP) X(OP_METHOD_PREP) X(OP_INVOKE) X(OP_ASSIGN)        \
    X(OP_ASSIGN_DROP) X(OP_THROW)

/* The global r names when r's cache still holds: else NULL, and global_ref finds it (and may run code). */
static inline Value *global_cached(Interp *I, const VarRef *r) {
    ObjMap *g = I->globals;
    int at = r->gslot - 1;
    return at >= 0 && (size_t)at < g->used && g->entries[at].live && g->entries[at].key.as.obj == (Obj *)r->name
               ? &g->entries[at].value : NULL;
}

/* What execute() returns when vm_step, op_return or vm_unwind says the frame it was entered for is done. */
static Value vm_result(Interp *I, int st) {
    Value r = ERR_VAL;
    if (st == VM_DONE) {
        r = I->ret_val;
        I->ret_val = NIL_VAL;
    }
    return r;
}

/* How the loop goes from one instruction to the next: by tail calls, where the compiler can promise them and has
 * a calling convention that leaves the VM's registers alone across the calls an instruction makes (clang's
 * musttail and preserve_none); else by computed goto (GNU C); else by a switch (C99). The same code each way. */
#if VM_TAILCALL && VM_THREADED && VM_REGS && VM_DBG_FOLD && defined(__clang__) && defined(__has_attribute)
#if __has_attribute(musttail) && __has_attribute(preserve_none)  /* (gcc 15 has both too: not tried yet) */
#define VM_TAIL 1
#endif
#endif
#ifndef VM_TAIL
#define VM_TAIL 0
#endif

#if VM_TAIL
/* ---------- each instruction a function, which ends by calling the next one's ---------- */
typedef struct VmTab VmTab;
#define VM_CC __attribute__((preserve_none))
#define VM_PARAMS Interp *I, Frame *f, const Instr *ip, Value *sp, Value *base, Value *consts, Instr in, const VmTab *tab
#define VM_ARGS I, f, ip, sp, base, consts, in, tab
typedef VM_CC Value VmOp(VM_PARAMS);
struct VmTab {
    VmOp *op[256];
};
#define VM_OP(op) static VM_CC Value vmop_##op(VM_PARAMS)
#define VM_OP2(a, b) VM_OP(a)
#define VM_PART(name) LUBE_NOINLINE static VM_CC Value vmpart_##name(VM_PARAMS)
#define GO_PART(name) do { __attribute__((musttail)) return vmpart_##name(VM_ARGS); } while (0)
#define RELEASE_AT(v)                                                \
    do {                                                             \
        if ((v).type == VAL_OBJ && --(v).as.obj->rc == 0) {          \
            I->dead = (v).as.obj; /* (its last owner: freed there) */ \
            __attribute__((musttail)) return vm_free_at(VM_ARGS);    \
        }                                                            \
        NEXT_AT();                                                   \
    } while (0)
#define NEXT_AT()                                                     \
    do {                                                              \
        in = *ip;                                                     \
        STEP_PC();                                                    \
        COUNT_FAST();                                                 \
        __attribute__((musttail)) return tab->op[OP_OF(in)](VM_ARGS); \
    } while (0)
#define NEXT(n)                      \
    do {                             \
        ip += (n);                   \
        NEXT_AT();                   \
    } while (0)
#define GO_SLOW() do { __attribute__((musttail)) return vm_slow(VM_ARGS); } while (0)
#define GO_RAISE() do { __attribute__((musttail)) return vm_raise(VM_ARGS); } while (0)
#define GO_CALLED(x)                                             \
    do {                                                         \
        in = (Instr)(x); /* (as op_call returned) */             \
        __attribute__((musttail)) return vm_called(VM_ARGS);     \
    } while (0)
#define GO_RETURNED(x)                                           \
    do {                                                         \
        in = (Instr)(x); /* (as op_return returned) */           \
        __attribute__((musttail)) return vm_returned(VM_ARGS);   \
    } while (0)

/* The instruction at ip, untouched, goes the long way. */
LUBE_NOINLINE static VM_CC Value vm_slow(VM_PARAMS) {
    COUNT_SLOW();
    SYNC();
    do {
        int st = vm_step(I);
        if (st != VM_NEXT) return vm_result(I, st);
    } while (I->dbg);  /* (the debugger looks before each instruction: none are run here meanwhile) */
    LOAD_FRAME();
    NEXT_AT();
}

/* What the instruction at ip called raised an error (the Interp has the VM's state). */
LUBE_NOINLINE static VM_CC Value vm_raise(VM_PARAMS) {
    int st = vm_unwind(I);
    if (st != VM_NEXT) return vm_result(I, st);
    LOAD_FRAME();
    NEXT_AT();
}

LUBE_NOINLINE static VM_CC Value vm_called(VM_PARAMS) {  /* (after op_call or op_invoke, whose answer is in `in`) */
    int st = (int)in;
    if (st < 0) GO_RAISE();
    if (st) {  /* the call has a frame */
        LOAD_FRAME();
        NEXT_AT();
    }
    LOAD_SP();
    NEXT(1);
}

LUBE_NOINLINE static VM_CC Value vm_returned(VM_PARAMS) {  /* (after op_return, whose answer is in `in`) */
    int st = (int)in;
    if (LUBE_LIKELY(st == VM_NEXT)) {
        LOAD_FRAME();
        NEXT_AT();
    }
    if (st == VM_RAISED) GO_RAISE();
    return vm_result(I, st);
}

/* I->dead (an object with no owner left) is freed; then on to the instruction at ip. */
LUBE_NOINLINE static VM_CC Value vm_free_at(VM_PARAMS) {
    obj_destroy(I->dead);
    NEXT_AT();
}

#include "interp_ops.h"

#define vmop_OP_GLOBAL_VALUE vmop_OP_GLOBAL
#define vmop_OP_ASSIGN_DROP vmop_OP_ASSIGN
#define vmop_OP_OR vmop_OP_AND
#define vmop_OP_CALL_FN vmop_OP_CALL
#if !VM_SLIM_CALL
#define vmop_OP_RETURN_NIL vmop_OP_RETURN
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"  /* (a range in an initializer: GNU C, used only where it is one) */
#pragma GCC diagnostic ignored "-Winitializer-overrides"
static const VmTab vm_tab = {{
    [0 ... 255] = vm_slow,
#define LUBE_OP_FN(op) [op] = vmop_##op,
    VM_FAST_OPS(LUBE_OP_FN)
#undef LUBE_OP_FN
}};
#pragma GCC diagnostic pop

/* Runs bytecode from I->frame until the entry frame it starts in returns,
 * and returns that frame's result (or ERR_VAL). */
static Value execute(Interp *I) {
    Frame *f = I->frame;
    if (!lube_stack_ok(I)) return leave(I, lube_error(I, "stack overflow: too much recursion"));
    while (I->dbg) {  /* the debugger looks before each instruction: all of them go the long way */
        int st = vm_step(I);
        if (st != VM_NEXT) return vm_result(I, st);
    }
#ifdef VM_STATS
    static int stats_on;
    if (!stats_on) {
        stats_on = 1;
        atexit(vm_stats_print);
    }
#endif
    const Instr *ip;
    Value *sp, *base, *consts;
    const VmTab *tab = &vm_tab;
#ifdef VM_PC_CHECK
    int was_synced = vm_synced;
    LOAD_FRAME();
    Value checked = tab->op[OP_OF(*ip)](I, f, ip, sp, base, consts, *ip, tab);
    vm_synced = was_synced;  /* (whatever called this loop goes on where it was) */
    return checked;
#else
    LOAD_FRAME();
    Instr in = *ip;
    STEP_PC();
    COUNT_FAST();
    return tab->op[OP_OF(in)](VM_ARGS);
#endif
}

#else
/* ---------- one function: each instruction a label (computed goto), or a case of a switch ---------- */
#if VM_THREADED
#define CASE(op) case op: L_##op
#define NEXT_AT()                    \
    do {                             \
        in = *ip;                    \
        STEP_PC();                   \
        COUNT_FAST();                \
        DBG_CHECK();                 \
        goto *run_tab[OP_OF(in)];    \
    } while (0)
#else
#define CASE(op) case op
#define NEXT_AT() goto vm_next
#endif
#define NEXT(n)                      \
    do {                             \
        ip += (n);                   \
        NEXT_AT();                   \
    } while (0)
#define VM_OP(op) CASE(op):
#define VM_OP2(a, b) CASE(a): CASE(b):
#define VM_PART(name) vmpart_##name:
#define GO_PART(name) goto vmpart_##name
#define RELEASE_AT(v)                \
    do {                             \
        release(v);                  \
        NEXT_AT();                   \
    } while (0)
#define GO_SLOW() goto slow
#define GO_RAISE() goto raise
#define GO_CALLED(x)                 \
    do {                             \
        st = (x);                    \
        goto called;                 \
    } while (0)
#define GO_RETURNED(x)               \
    do {                             \
        st = (x);                    \
        goto returned;               \
    } while (0)

/* Runs bytecode from I->frame until the entry frame it starts in returns,
 * and returns that frame's result (or ERR_VAL). */
#if VM_THREADED
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"  /* (labels as values: GNU C, used only where it is one) */
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Winitializer-overrides"
#else
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
#endif
static Value execute(Interp *I) {
    Frame *f = I->frame;
    if (!lube_stack_ok(I)) return leave(I, lube_error(I, "stack overflow: too much recursion"));
    const Instr *ip;
    Instr in = 0;
    int st;
#if VM_REGS
    Value *sp, *base, *consts;
#endif
#if VM_THREADED
    static const void *const run_tab[256] = {
        [0 ... 255] = &&slow,
#define LUBE_OP_LABEL(op) [op] = &&L_##op,
        VM_FAST_OPS(LUBE_OP_LABEL)
#undef LUBE_OP_LABEL
    };
#endif
#ifdef VM_STATS
    static int stats_on;
    if (!stats_on) {
        stats_on = 1;
        atexit(vm_stats_print);
    }
#endif
    LOAD_FRAME();
    if (I->dbg) goto step;  /* the debugger looks before each instruction: all of them go the long way */
    for (;;) {
#if VM_THREADED
        NEXT_AT();  /* (as every instruction here ends: the switch below is only its labels) */
#else
    vm_next:
        in = *ip;
        STEP_PC();
        COUNT_FAST();
        DBG_CHECK();
#endif
        switch ((OpCode)OP_OF(in)) {
#include "interp_ops.h"
        default:
            goto slow;
        }

    called:  /* (after op_call or op_invoke) */
        if (st < 0) goto raise;
        if (st) {  /* the call has a frame */
            LOAD_FRAME();
            NEXT_AT();
        }
        LOAD_SP();
        NEXT(1);

    returned:  /* (after op_return) */
        if (LUBE_LIKELY(st == VM_NEXT)) {
            LOAD_FRAME();
            NEXT_AT();
        }
        if (st != VM_RAISED) return vm_result(I, st);

    raise:  /* what the instruction at ip called raised an error (the Interp has the VM's state) */
        st = vm_unwind(I);
        if (st != VM_NEXT) return vm_result(I, st);
        LOAD_FRAME();
        NEXT_AT();

    slow:  /* the instruction at ip, untouched, goes the long way */
        COUNT_SLOW();
        SYNC();
    step:
        do {
            st = vm_step(I);
            if (st != VM_NEXT) return vm_result(I, st);
        } while (I->dbg);  /* (the debugger looks before each instruction: none are run here meanwhile) */
        LOAD_FRAME();
    }
}
#if VM_THREADED
#pragma GCC diagnostic pop
#endif
#endif  /* VM_TAIL */


/* Calls a function or built-in from C with the argc arguments at
 * stack[base...], which the caller owns (and pops afterwards). (Callbacks
 * must be functions: a class is never called this way.) */
static Value call_value(Interp *I, Value callee, int argc, int base) {
    Value r;
    if (IS_FUNCTION(callee) && AS_FUNCTION(callee)->decl->native && call_native(I, AS_FUNCTION(callee)->decl, base, argc, &r))
        return r;
    if (IS_FUNCTION(callee)) {  /* (a plain function needs none of enter()'s checks, as in OP_CALL) */
        ObjFunction *fn = AS_FUNCTION(callee);
        FuncDecl *d = fn->decl;
        Frame *f = d->plain && plain_args(I, d, base, argc, 0)
                       ? push_frame(I, fn, d->name ? d->name->chars : "<lambda>", base, argc) : enter(I, fn, base, argc);
        if (!f) return ERR_VAL;
        f->entry = 1;
        FRAME_SLOW_RET(f);
        f->this_locked = I->this_locked_next;  /* lube_call_method: `this` cannot change */
        I->this_locked_next = 0;
        return execute(I);
    }
    ObjNative *nf = AS_NATIVE(callee);
    if (argc < nf->min_args || (nf->max_args >= 0 && argc > nf->max_args))
        return arity_error(I, nf->name, nf->min_args, nf->max_args, argc);
    return (I->calling = nf)->fn(I, argc, &I->stack[base]);
}

/* Runs top-level code (a script, a module or a hot reload) in an entry frame. */
static Value run_top(Interp *I, FuncDecl *d) {
    int base = I->sp;
    if (VM_DEPTH(I) >= LUBE_MAX_FRAMES || base + d->nslots + d->code.maxstack > LUBE_STACK_MAX)
        return lube_error(I, "stack overflow: too much recursion"); /* LCOV_EXCL_LINE: imports nest only so deep */
    for (int i = 0; i < d->nslots; i++) I->stack[base + i] = NIL_VAL;
    I->sp = base + d->nslots;
    Frame *f = new_frame(I);
    frame_init(I, f, I->frame, NULL, d, d->name->chars, 1, base, 0, base, base + d->nslots);
    f->entry = 1;
    FRAME_SLOW_RET(f);
    I->frame = f;
    return execute(I);
}

/* Parses, checks and compiles a source; NULL (with the error reported) if
 * it has errors. (A program built with --small has no compiler: it loads
 * the file as compiled when it was built.) */
static FuncDecl *compile_source(Interp *I, Source *src, const char *name, int mode) {
#ifdef LUBE_RUNTIME
    FuncDecl *d = lube_image_load(I, src, mode);
    if (!d) {  /* LCOV_EXCL_START: a damaged executable */
        fprintf(stderr, "lube: this executable has no compiled code for '%s'\n", src->path);
        return NULL;
    }  /* LCOV_EXCL_STOP */
    d->name = lube_intern(I, name, strlen(name));
    lube_native_attach(I, d, src);
    return d;
#else
    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (!program) return NULL;
    if (!resolve_program(I, program, src, arena)) {
        arena_free(arena);
        return NULL;
    }
    keep_arena(I, arena);  /* functions defined here may outlive this run */
    FuncDecl *d = compile_program(I, program, src, arena, name, mode);
    lube_native_attach(I, d, src);  /* (lube --build --native: machine code for some of its fns) */
    return d;
#endif
}

FuncDecl *lube_compile_source(Interp *I, Source *src, const char *name, int mode) {
    return compile_source(I, src, name, mode);
}

/* Drops "." segments and folds "dir/.." in a path, so that one file always
 * has one name (and is imported once, however it is reached). */
static void tidy_path(StrBuf *p) {
    char *d = p->data;
    size_t n = p->len, o = 0, i = 0, nseg = 0;
    size_t *starts = xmalloc(sizeof(size_t) * (n + 1));  /* where each kept segment begins */
    if (d[0] == '/') o = i = 1;
    size_t root = o;
    while (i < n) {
        size_t j = i;
        while (j < n && d[j] != '/') j++;
        size_t len = j - i;
        int up = len == 2 && d[i] == '.' && d[i + 1] == '.';
        size_t prev = nseg ? starts[nseg - 1] : 0;
        if (up && nseg && !(o - prev == 3 && d[prev] == '.' && d[prev + 1] == '.')) {
            o = starts[--nseg];  /* "dir/.." cancels out */
        } else if (len && !(len == 1 && d[i] == '.')) {
            starts[nseg++] = o;
            memmove(d + o, d + i, len);
            o += len;
            d[o++] = '/';
        }
        i = j + 1;
    }
    if (o > root) o--;  /* no slash at the end */
    d[o] = '\0';
    p->len = o;
    free(starts);
}

/* The file `import "rel"` means in the file `importer`: relative to the
 * importer's folder, with .lube added when the name has no extension. A
 * built program finds the same paths in its bundle. */
void lube_module_path(StrBuf *path, const char *importer, const char *rel) {
    if (rel[0] != '/') {
        const char *slash = strrchr(importer, '/');
        if (slash) sb_putn(path, importer, (size_t)(slash - importer + 1));
    }
    sb_puts(path, rel);
    const char *last = strrchr(path->data, '/');
    if (!strchr(last ? last : path->data, '.')) sb_puts(path, ".lube");
    tidy_path(path);
}

/* ======================================================================
 * Packages: `package a.b` at the top of every file in the folder a/b. Their
 * globals are named "a.b::name", so each package has names of its own.
 * ==================================================================== */

/* The global a package's name is kept in: a.b::name. */
ObjString *lube_global_key(Interp *I, ObjString *pkg, ObjString *name) {
    StrBuf sb;
    sb_init(&sb);
    sb_putn(&sb, pkg->chars, pkg->length);
    sb_puts(&sb, "::");
    sb_putn(&sb, name->chars, name->length);
    ObjString *key = lube_intern(I, sb.data, sb.len);
    sb_free(&sb);
    return key;
}

/* A global's name for messages: Point for lube.geometry::Point. */
const char *lube_shown(ObjString *key) {
    const char *p = strstr(key->chars, "::");
    return p ? p + 2 : key->chars;
}

/* The .lube files directly in a folder of the bundle (a built program). */
static char **bundle_files(Interp *I, const char *folder, int *n) {
    size_t len = strlen(folder);
    char **names = NULL;
    int cap = 0;
    *n = 0;
    for (size_t i = 0; i < I->bundle->used; i++) {
        ObjString *k = AS_STRING(I->bundle->entries[i].key);
        if (k->length <= len + 1 || strncmp(k->chars, folder, len) || k->chars[len] != '/' || strchr(k->chars + len + 1, '/'))
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            names = xrealloc(names, sizeof(char *) * (size_t)cap);
        }
        names[(*n)++] = xstrndup(k->chars + len + 1, k->length - len - 1);
    }
    return names;  /* in the order they were built: sorted already */
}

/* The .lube files of a package's folder (names only, sorted). */
char **lube_package_files(Interp *I, const char *folder, int *n) {
    return I->bundle ? bundle_files(I, folder, n) : os_lube_files(folder, n);
}

static void free_names(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* Finds package a.b.c: the folder a/b/c, with .lube files in it, under the
 * program's folder or a folder of LUBE_PATH (in a built program: in it).
 * 1 if found, with its path in *folder. */
int lube_find_package(Interp *I, ObjString *pkg, StrBuf *folder) {
    StrBuf rel;
    sb_init(&rel);
    for (size_t i = 0; i < pkg->length; i++) sb_putc(&rel, pkg->chars[i] == '.' ? '/' : pkg->chars[i]);
    const char *path = getenv("LUBE_PATH");
    const char *root = I->bundle ? NULL : I->root ? I->root : ".";
    int found = 0;
    for (;;) {
        folder->len = 0;
        if (root) {
            sb_puts(folder, root);
            sb_putc(folder, '/');
        }
        sb_putn(folder, rel.data, rel.len);
        int n;
        char **names = lube_package_files(I, folder->data, &n);
        free_names(names, n);
        if (n > 0) {
            found = 1;
            break;
        }
        if (I->bundle || !path || !*path) break;  /* next: the folders of LUBE_PATH */
        const char *end = strchr(path, LUBE_PATH_SEP);
        size_t len = end ? (size_t)(end - path) : strlen(path);
        static LUBE_THREAD_LOCAL char dir[4096];
        snprintf(dir, sizeof dir, "%.*s", (int)len, path);
        lube_slashes(dir);
        root = dir;
        path = end ? end + 1 : NULL;
    }
    sb_free(&rel);
    return found;
}

/* Reads one file of a package (from the bundle in a built program). */
static char *package_text(Interp *I, const char *path, size_t *len) {
    if (!I->bundle) return read_file(path, len);
    Value text;
    ObjString *key = new_string(path, strlen(path));
    int ok = map_get(I->bundle, OBJ_VAL(key), &text);
    release(OBJ_VAL(key));
    if (!ok) return NULL; /* LCOV_EXCL_LINE: the bundle listed it */
    *len = AS_STRING(text)->length;
    return xstrndup(AS_STRING(text)->chars, *len);
}

/* The top-level names package pkg declares, as a map name -> true (cached),
 * or NULL when there is no such package. *bad: one of its files has errors
 * (reported). */
#ifndef LUBE_RUNTIME  /* (the resolver's, and --check's: not in a program built with --small, which has no compiler) */
ObjMap *lube_package_names(Interp *I, ObjString *pkg, int *bad) {
    Value cached;
    *bad = 0;
    if (!I->pkg_names) I->pkg_names = new_map();
    if (map_get(I->pkg_names, OBJ_VAL(pkg), &cached)) return IS_MAP(cached) ? AS_MAP(cached) : NULL;
    StrBuf folder;
    sb_init(&folder);
    ObjMap *names = NULL;
    if (lube_find_package(I, pkg, &folder)) {
        int n;
        char **files = lube_package_files(I, folder.data, &n);
        names = new_map();
        for (int i = 0; i < n; i++) {
            StrBuf path;
            sb_init(&path);
            sb_printf(&path, "%s/%s", folder.data, files[i]);
            size_t len;
            char *text = package_text(I, path.data, &len);
            Source *src = lube_add_source(I, path.data, text, len);
            sb_free(&path);
            Arena *arena;
            Node *program = parse_program(I, src, &arena);
            if (!program) {
                *bad = 1;
                continue;
            }
            if (src->package != pkg) {
                report_error(src, 1, 0, "error", src->package ? "this file is in the folder of another package than its package line says"
                                                              : "this file is in a package's folder, so it must start with its package line");
                *bad = 1;
            }
            NodeList *stmts = &program->as.block.stmts;
            for (int j = 0; j < stmts->count; j++) {
                Node *s = stmts->items[j];
                ObjString *name = s->kind == N_LET ? s->as.let.var.name : s->kind == N_CLASS ? s->as.klass.var.name : NULL;
                if (name) map_set(names, retain(OBJ_VAL(name)), BOOL_VAL(1));
            }
            arena_free(arena);
        }
        free_names(files, n);
    } else {  /* no such folder: perhaps a plugin */
        names = lube_plugin_names(I, pkg, bad);
    }
    sb_free(&folder);
    if (*bad) {
        if (names) release(OBJ_VAL(names));
        return NULL;
    }
    map_set(I->pkg_names, retain(OBJ_VAL(pkg)), names ? OBJ_VAL(names) : BOOL_VAL(0));
    return names;
}
#endif

/* import a.b...: loads package pkg (once): every file in its folder. With
 * `bind` (import a.b), the package itself goes in that global. */
static int import_package(Interp *I, ObjString *pkg, ObjString *bind) {
    if (!map_find(I->packages, OBJ_VAL(pkg))) {
        map_set(I->packages, retain(OBJ_VAL(pkg)), BOOL_VAL(1));
        StrBuf folder;
        sb_init(&folder);
        if (!lube_find_package(I, pkg, &folder)) {  /* no folder: a plugin (found when compiling the import) */
            sb_free(&folder);
            if (lube_plugin_import(I, pkg) < 0) return -1;
            goto bound;
        }
        int n;
        char **files = lube_package_files(I, folder.data, &n);
        int failed = 0;
        for (int i = 0; i < n && !failed; i++) {
            StrBuf path;
            sb_init(&path);
            sb_printf(&path, "%s/%s", folder.data, files[i]);
            size_t len;
            char *text = package_text(I, path.data, &len);
            if (!I->bundle) map_set(I->watch, OBJ_VAL(new_string(path.data, path.len)), OBJ_VAL(new_string(text, len)));
            Source *src = lube_add_source(I, path.data, text, len);
            sb_free(&path);
            FuncDecl *d = compile_source(I, src, "<package>", I->isolate ? RUN_ISOLATE : RUN_FILE);
            if (!d) {
                lube_error(I, "cannot import package %s: %s has errors", pkg->chars, src->path);
                failed = 1;
            } else if (IS_ERR(run_top(I, d))) {
                failed = 1;
            }
        }
        free_names(files, n);
        sb_free(&folder);
        if (failed) return -1;
    }
bound:
    if (bind) {
        ObjPackage *o = (ObjPackage *)alloc_object(sizeof(ObjPackage), OBJ_PACKAGE);
        o->name = (ObjString *)retain(OBJ_VAL(pkg)).as.obj;
        if (define_global(I, bind, OBJ_VAL(o), 1) < 0) return -1;
    }
    return 0;
}

/* pkg.name: a public global of a package. */
static Value package_member(Interp *I, Value pkgv, ObjString *name) {
    ObjString *pkg = AS_PACKAGE(pkgv)->name;
    if (name->chars[0] == '_') return lube_error(I, "'%s' is private to package %s", name->chars, pkg->chars);
    int failed;
    Value *g = lookup_global(I, lube_global_key(I, pkg, name), &failed);
    if (failed) return ERR_VAL;
    if (!g) return lube_error(I, "package %s has no '%s'", pkg->chars, name->chars);
    return retain(*g);
}

/* import "rel": runs the module (once) in a frame of its own. */
static int do_import(Interp *I, ObjString *rel) {
    StrBuf path;
    sb_init(&path);
    Source *from = lube_frame_src(I->frame);
    lube_module_path(&path, from ? from->path : "", rel->chars);
    Value key = OBJ_VAL(new_string(path.data, path.len));
    if (map_find(I->modules, key)) {  /* each file runs only once */
        release(key);
        sb_free(&path);
        return 0;
    }
    size_t len;
    char *text = NULL;
    Value packed;
    if (!I->bundle) {
        text = read_file(path.data, &len);
    } else if (map_get(I->bundle, key, &packed)) {  /* a built program carries every module it imports */
        len = AS_STRING(packed)->length;
        text = xstrndup(AS_STRING(packed)->chars, len);
    }
    if (!text) {
        release(key);
        sb_free(&path);
        lube_error(I, "cannot import '%s': file not found", rel->chars);
        return -1;
    }
    map_set(I->modules, key, BOOL_VAL(1));
    if (!I->bundle) map_set(I->watch, retain(key), OBJ_VAL(new_string(text, len)));  /* for hot reload */
    Source *src = lube_add_source(I, path.data, text, len);
    sb_free(&path);
    FuncDecl *d = compile_source(I, src, "<module>", I->isolate ? RUN_ISOLATE : RUN_FILE);
    if (!d) {
        lube_error(I, "cannot import '%s': the file has errors", rel->chars);
        return -1;
    }
    return IS_ERR(run_top(I, d)) ? -1 : 0;
}



/* ======================================================================
 * Running programs
 * ==================================================================== */

LUBE_COLD void lube_report_uncaught(Interp *I) {
    fflush(stdout);
    Value e = I->error_val;
    I->error_val = NIL_VAL;
    if (I->exiting) {  /* (hosted: exit() ended it, which is no error) */
        release(e);
        trace_reset(I);
        return;
    }
    ObjString *msg = IS_TEXT(e) ? (ObjString *)retain(OBJ_VAL(text_of(e))).as.obj : value_repr(e);
    Source *src = I->error_src;
    if (src) lube_print(1, "%s:%d: error: %s\n", src->path, I->error_line, msg->chars);
    else lube_print(1, "error: %s\n", msg->chars);  /* (raised with no Lube code running: a host program's call) */
    print_source_line(src, I->error_line, 0);
    trace_flush_repeats(I);
    if (I->trace_count > 1) {
        lube_print(1, "traceback (innermost call first):\n%s", I->trace.data ? I->trace.data : "");
        if (I->trace_hidden > 0) {
            if (I->trace_hidden > 1) lube_print(1, "  ... %d more calls ...\n", I->trace_hidden - 1);
            lube_print(1, "%s", I->trace_last);
        }
    }
    release(OBJ_VAL(msg));
    release(e);
    trace_reset(I);
}

/* ======================================================================
 * Hot reload (lube --hot, and hot_reload())
 *
 * When a file the program was loaded from changes, its declarations are
 * applied to the running program: functions are replaced, classes get their
 * new methods, fields and constructor (existing instances included), new
 * globals are made and new imports run. Variables that already exist keep
 * their values, and no other statement runs again, so a running loop keeps
 * going with the new code. Replaced functions and classes stay alive until
 * shutdown, since a call to one may still be running.
 * ==================================================================== */


/* Applies a changed file. Errors are reported, and the program goes on
 * with the code it had. Returns 1 if the file was applied. */
static int hot_apply(Interp *I, Value path, ObjString *text) {
    map_set(I->watch, retain(path), retain(OBJ_VAL(text)));  /* an error is reported once, not on every look */
    Source *src = lube_add_source(I, AS_STRING(path)->chars, xstrndup(text->chars, text->length), text->length);
    if (I->pkg_names) {  /* a package file may have changed what its package declares */
        release(OBJ_VAL(I->pkg_names));
        I->pkg_names = NULL;
    }
    FuncDecl *d = compile_source(I, src, "<reload>", RUN_HOT);
    if (!d) {
        lube_print(1, "lube: %s not reloaded: fix the error above\n", src->path);
        return 0;
    }
    for (int i = 0; i < I->nwatchers; i++) {  /* the file's `on` blocks are made again */
        Watcher *w = &I->watchers[i];
        if (w->nnames && !strcmp(w->file, src->path)) {
            w->nnames = 0;
            list_push(I->hot_old, w->fn);  /* it may be running */
            w->fn = NIL_VAL;
        }
    }
    I->hot_busy = 1;
    Value r = run_top(I, d);
    I->hot_busy = 0;
    if (IS_ERR(r)) {
        lube_report_uncaught(I);
        lube_print(1, "lube: %s reloaded only up to the error above\n", src->path);
        return 0;
    }
    fflush(stdout);
    lube_print(1, "lube: reloaded %s\n", src->path);
    return 1;
}

/* Looks at every file the program was loaded from and applies the ones
 * that changed. Returns the paths applied. */
ObjList *lube_hot_reload(Interp *I) {
    ObjList *done = new_list();
    done->elem = TT_STRING;
    if (I->hot_busy) return done;
    ObjMap *w = I->watch;
    for (size_t i = 0; i < w->used; i++) {
        if (!w->entries[i].live) continue;
        Value path = w->entries[i].key;
        size_t len;
        char *now = read_file(AS_STRING(path)->chars, &len);
        if (!now) continue;  /* being saved, or gone: look again later */
        ObjString *was = AS_STRING(w->entries[i].value);
        if (len != was->length || memcmp(now, was->chars, len)) {
            ObjString *text = new_string(now, len);
            retain(path);  /* applying can change I->watch */
            if (hot_apply(I, path, text)) list_push(done, retain(path));
            release(path);
            release(OBJ_VAL(text));
        }
        free(now);
    }
    return done;
}

/* With --hot, looks at the files every 50 ms of running time. */
static void hot_check(Interp *I) {
    I->hot_ticks = 0;
    long now = (long)clock();
    if (now >= I->hot_last && now - I->hot_last < (long)(CLOCKS_PER_SEC / 20)) return;
    I->hot_last = now;
    release(OBJ_VAL(lube_hot_reload(I)));
}

/* Parses and runs a source. In REPL mode the value of a final expression is
 * printed. Returns a process exit status. */
int lube_run_source(Interp *I, Source *src, int repl) {
    FuncDecl *d = compile_source(I, src, "<script>", repl ? RUN_REPL : RUN_FILE);
    if (!d) return 65;
    lube_note_program(I, src->path, src->text, src->length);  /* what a spawned isolate loads */
    if (IS_ERR(run_top(I, d))) {
        lube_report_uncaught(I);
        return 70;
    }
    return 0;
}

/* In a new isolate: runs a program file's declarations (functions, classes,
 * imports, global variables, `on` blocks). 0, 65 (it has errors) or 70 (an
 * error was raised, left in I->error_val). */
int lube_run_declarations(Interp *I, const char *path, const char *text, size_t len) {
    Source *src = lube_add_source(I, path, xstrndup(text, len), len);
    FuncDecl *d = compile_source(I, src, "<isolate>", RUN_ISOLATE);
    if (!d) return 65;
    lube_note_program(I, path, text, len);
    return IS_ERR(run_top(I, d)) ? 70 : 0;
}

/* Checks a program without running any of it: syntax and resolver errors
 * (the same ones that stop a run with status 65). Returns 0 or 65. */
#ifndef LUBE_RUNTIME
int lube_check_source(Interp *I, Source *src) {
    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (!program) return 65;
    int ok = resolve_program(I, program, src, arena);
    arena_free(arena);
    return ok ? 0 : 65;
}
#endif

Source *lube_add_source(Interp *I, const char *path, char *text, size_t length) {
    Source *s = xmalloc(sizeof(Source));
    s->path = xstrndup(path, strlen(path));
    s->text = text;
    s->length = length;
    s->strict = 0;
    s->package = NULL;
    s->aliases = NULL;
    s->next = I->sources;
    I->sources = s;
    return s;
}

int lube_run_file(Interp *I, const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        lube_print(1, "lube: cannot open file '%s'\n", path);
        return 66;
    }
    map_set(I->watch, OBJ_VAL(new_string(path, strlen(path))), OBJ_VAL(new_string(text, len)));  /* for hot reload */
    if (!I->root) {  /* packages are found from the program's folder */
        const char *slash = strrchr(path, '/');
        I->root = slash ? xstrndup(path, (size_t)(slash - path)) : xstrndup(".", 1);
    }
    return lube_run_source(I, lube_add_source(I, path, text, len), 0);
}

/* Defines a built-in global (cannot be assigned to). Takes over v. */
void lube_define(Interp *I, const char *name, Value v) {
    ObjString *key = lube_intern(I, name, strlen(name));
    map_set(I->globals, retain(OBJ_VAL(key)), v);
    map_set(I->constants, retain(OBJ_VAL(key)), NUM_VAL(ORIGIN_BUILTIN));
    I->epoch++;
}

Interp *lube_new(char *stack_base) {
    Interp *I = lube_pages(sizeof(Interp));  /* (zeroed: most of it is the stack, touched only as deep as it goes) */
    I->ret_val = NIL_VAL;
    I->error_val = NIL_VAL;
    I->epoch = 1;  /* (zeroed caches are stale) */
    I->stack_base = stack_base;
    size_t margin = LUBE_STACK_SIZE / 8 > 512u * 1024u ? LUBE_STACK_SIZE / 8 : 512u * 1024u;
    I->stack_budget = LUBE_STACK_SIZE - margin;
    I->rng = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)I << 16) ^ 0x9E3779B97F4A7C15ULL;
    if (I->rng == 0) I->rng = 1;
    sb_init(&I->trace);

    I->decl = lube_pages(sizeof(TypeDesc) * LUBE_STACK_MAX);
    I->strings = new_map();
    I->globals = new_map();
    I->constants = new_map();
    I->modules = new_map();
    I->global_type_index = new_map();
    lube_plugins_start();
    I->classes = new_list();
    I->watch = new_map();
    I->hot_old = new_list();
    I->packages = new_map();
    I->strings->internal = I->globals->internal = I->constants->internal = I->modules->internal = 1;
    I->global_type_index->internal = I->watch->internal = I->packages->internal = 1;
    for (int c = 0; c < 256; c++) {
        char ch = (char)c;
        I->chars[c] = new_string(&ch, 1);
    }
    register_builtins(I);
    register_reflection(I);
    register_isolates(I);
    register_io(I);
    register_money(I);
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
    for (int i = 0; i < I->nwatchers; i++) release(I->watchers[i].fn);
    free(I->watchers);
    release(OBJ_VAL(I->hot_old));
    release(OBJ_VAL(I->watch));
    if (I->lazy) release(OBJ_VAL(I->lazy));
    release(OBJ_VAL(I->packages));
    if (I->pkg_names) release(OBJ_VAL(I->pkg_names));
    if (I->main_aliases) release(OBJ_VAL(I->main_aliases));
    free(I->root);
    release(OBJ_VAL(I->classes));  /* after every instance is gone */
    free(I->foreign_classes);      /* (the classes were in I->classes) */
    if (I->bundle) release(OBJ_VAL(I->bundle));
    if (I->imports_found) release(OBJ_VAL(I->imports_found));
    free(I->global_types);
    lube_pages_free(I->decl, sizeof(TypeDesc) * LUBE_STACK_MAX);
    for (int c = 0; c < 256; c++) release(OBJ_VAL(I->chars[c]));
    for (int t = 0; t < LUBE_TM_TYPES; t++)
        if (I->tmethods[t]) release(OBJ_VAL(I->tmethods[t]));
    for (int i = 0; i < I->narenas; i++) arena_free(I->arenas[i]);
    free(I->arenas);
    release(OBJ_VAL(I->strings));  /* last: the syntax trees borrowed these */
    Source *s = I->sources;
    while (s) {
        Source *next = s->next;
        if (s->aliases) release(OBJ_VAL(s->aliases));
        free(s->path);
        free(s->text);
        free(s);
        s = next;
    }
    sb_free(&I->trace);
    sb_free(&I->interp);
    for (int i = 0; i < I->nchunks; i++) free(I->chunks[i]);
    free(I->chunks);
    free(I->pending);
    lube_free_program(I);
    for (int i = 0; i < I->nloan_pool; i++) free(I->loan_pool[i]);
    free(I->loan_pool);
    lube_pages_free(I, sizeof(Interp));
    return lube_live_objects;
}
