/*
 * native.c - lube --build --native: typed functions compiled to machine code.
 *
 * A top-level fn whose parameters and result are Numbers and Bools, and whose
 * body only computes with them (arithmetic, comparisons, and/or/not, if,
 * while, for over a range, calls to other such functions and to the math
 * built-ins), is translated to C and compiled, together with Lube's own
 * sources, by the system's C compiler. The VM then calls its machine code
 * instead of running its bytecode; everything else runs as before.
 *
 * The machine code computes exactly what the bytecode would: the same
 * double arithmetic, Lube's own %, the same range lengths. It never reports
 * an error itself. Where one would happen (division by zero, a fn that ends
 * without returning its value, calls nested too deeply) it gives up, and the
 * VM runs the whole call again, which raises the error as usual. That is
 * safe because these functions change nothing outside themselves.
 *
 * Records (objects whose Number fields a fn uses, and lists of them) go in
 * as arrays of those fields, laid out alike in fns that pass them to each
 * other (see Gen.shapes). Methods are such fns, `this` a record of exactly
 * their class. A loop at the top level of a file is one too: the globals it
 * uses are its parameters, which its wrapper takes in and writes back when
 * it is done (OP_NATIVE_LOOP runs it, and its bytecode when it gives up).
 *
 * Each such fn has two versions. The first works on doubles, as the VM
 * does. The second, used when every Number it is given is a whole number,
 * works on 64-bit integers wherever the numbers are whole: they are exact
 * while within 2^53 (as in a double), so it checks that bound, gives up past
 * it, and never makes -0 (which a double can be and an integer cannot). When
 * the whole-number version gives up, the double version runs, then the VM.
 */
#include "lube.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Types: NT_INT is a whole Number within 2^53 (held in an int64_t), NT_NUM
 * any Number (a double), NT_NONE a fn's result when it returns nothing.
 * NT_LNUM and NT_LBOOL are lists of Numbers and of Bools that the fn makes
 * itself and only indexes, changes, pushes to and measures: they never
 * leave it, so they are plain C arrays (Ln and Lb), freed when it returns. */
enum { NT_NONE, NT_NUM, NT_BOOL, NT_INT, NT_LNUM, NT_LBOOL, NT_TLNUM, NT_TLBOOL,  /* (TL: typed, Number[]) */
       NT_RLIST, NT_REC, NT_VAL };
/* NT_VAL: any Lube value (a C Value), for fns on lists of lists and the
 * like: a parameter's is borrowed, read with len(v) and v[i] (borrowed
 * too), passed on, and unboxed where a Number or Bool is wanted; a list
 * literal makes a new one, and a fn without a written result type may
 * return one. The ones a fn makes are its own (vt<k>, released on its way
 * out unless given away: returned, or put in a list). */
/* NT_RLIST and NT_REC: an untyped parameter used as a list of records
 * (xs[i].x, len(xs), for r in xs) or as a record (p.x): objects whose
 * fields it uses all hold Numbers. The wrapper the VM calls copies those
 * fields into an array (R, Bp), checking every object has them, and after
 * the machine code is done writes back to a & parameter the fields that
 * changed. A record taken out of one (let r = xs[i], or a loop's) is a copy. */
#define RECORD(t) ((t) == NT_RLIST || (t) == NT_REC)
#define UNTYPED (-2)  /* a parameter's type until its uses say which record it is */
#define NUMERIC(t) ((t) == NT_NUM || (t) == NT_INT)
#define LIST_NUM(t) ((t) == NT_LNUM || (t) == NT_TLNUM)  /* (a var keeps its list's kind: typed or not) */
#define LCHAR(t) (LIST_NUM(t) ? 'n' : 'b')
#define LTYPE(t) (LIST_NUM(t) ? "LubeNNum" : "LubeNBool")
#define NATIVE_MAX_PARAMS 8

typedef struct {
    FuncDecl *d;
    ObjString *key;     /* the fn's global */
    uint64_t hash;      /* of its file's text */
    int params[NATIVE_MAX_PARAMS], ret;
    int shape[NATIVE_MAX_PARAMS];  /* a record parameter's fields (Gen.shapes) */
    int ok;             /* native: its double version compiles (a fixpoint: callees must be native too) */
    int ok_int;         /* ...and its whole-number version */
    int why;            /* not native: the line where translating it stopped */
    int loop;           /* a top-level loop (key @N: the program's statement N): its parameters are the
                           globals it uses (loaded and written back by its wrapper), Numbers by pointer */
    int hint[NATIVE_MAX_PARAMS];     /* a loop's global: Bool or Number when used as a value */
    int written[NATIVE_MAX_PARAMS];  /* ...that it may change */
    int fixed[NATIVE_MAX_PARAMS];    /* ...declared with let */
    int method;         /* a method (key Class.name): parameter 0 is `this`, a record of exactly that class */
    int vals;           /* its untyped parameters are values (NT_VAL): tried when they cannot be records */
    int cls[NATIVE_MAX_PARAMS];      /* a record parameter's class + 1 (0: any), when methods are called on it
                                        (a list's: its records'): its wrapper checks it */
} Cand;

typedef struct {
    ObjString *key;     /* the global it is in */
    Node *node;         /* N_CLASS */
} ClassInfo;

typedef struct {
    Cand *cands;
    int ncands;
    ObjMap *declared;   /* every global the program's files declare: key -> how many times */
    ObjMap *vars;       /* the globals that are variables (let and var, not fns): key -> Bool or Number (+ 16: let) */
    ClassInfo *classes; /* the program's classes */
    int nclasses;
    Cand *cur;          /* the fn being translated */
    int mode;           /* 0: its double version; 1: its whole-number version */
    StrBuf *out;
    int tmp;            /* temporaries: t0, t1, ... */
    /* types belong to declarations (a var, a let, a loop's variable, a parameter: keyed by its VarRef or
     * name), over the whole fn (see gen_function); live[slot]: the declaration a slot holds now (-1: none) */
    void **dkeys;
    int *dtypes, ndecls, capdecls;
    int *dshape;        /* a record declaration's shape */
    unsigned char *dneg;  /* a whole-number declaration may be negative (else, so far, every value it got was not) */
    unsigned char *tnn;   /* tnn[k]: temporary t<k> is a whole number that is not negative */
    int captnn;
    int nvt;            /* the values this fn makes: vt0, vt1, ... */
    int fast;           /* lube --build --native --fast: whole numbers wrap around past 2^63, unchecked (not
                           2^53), and the C compiler may reorder sums: not always the VM's bits, quicker */
    int *live;
    int *acc;           /* acc[slot]: an accumulator of the loop with flag ovf<acc - 1> (see loop_accs), else 0 */
    /* shapes: the fields a record parameter's uses need, found as it is translated. A record
     * passed to another fn is laid out as that fn's parameter is: their shapes join (parent:
     * the shape it joined, or itself), and shape_changes counts what grew */
    struct { ObjString *fields[LUBE_REC_MAX]; int n, parent; } *shapes;
    int nshapes, shape_changes;
    int widened;        /* a slot's type changed in this pass: translate again */
    int failed;
    int line;           /* of the code being translated (failed: where it stopped) */
} Gen;

static int gen_expr(Gen *g, Node *n, char *res);
static void gen_stmt(Gen *g, Node *n);
static int writes_to(Node *n, int s);
static int is_value(Gen *g, Node *n);
static int gen_invoke(Gen *g, Cand *c, const char *fn, Node **argv, int argc, int self, char *res, int need);

uint64_t lube_hash_text(const char *text, size_t len) {
    uint64_t h = 1469598103934665603ull;  /* FNV-1a */
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)text[i]) * 1099511628211ull;
    return h;
}

static int ann_type(const TypeAnn *t) {
    if (!t->set) return NT_NONE;
    return t->d.kind == TD_NUM ? NT_NUM : t->d.kind == TD_BOOL ? NT_BOOL : -1;
}

static Cand *cand_for(Gen *g, ObjString *key) {
    for (int i = 0; i < g->ncands; i++)
        if (g->cands[i].key == key && g->cands[i].ok) return &g->cands[i];
    return NULL;
}

static int fail(Gen *g) {
    g->failed = 1;  /* (g->line stays where it stopped: nothing below runs on) */
    return 0;
}

static const char *bail(Gen *g) { (void)g; return "goto done;"; }  /* (R still says it gave up) */
static int new_tmp(Gen *g) {
    if (g->tmp == g->captnn) {
        g->captnn = g->captnn ? g->captnn * 2 : 64;
        g->tnn = xrealloc(g->tnn, (size_t)g->captnn);
    }
    g->tnn[g->tmp] = 0;
    return g->tmp++;
}
static char letter(int t) { return t == NT_NUM ? 'd' : t == NT_INT ? 'i' : 'b'; }
static const char *ctype(int t) { return t == NT_NUM ? "double" : t == NT_INT ? "int64_t" : "int"; }

/* e (of type t) as a double. */
static void as_double(int t, const char *e, char *out) { snprintf(out, 64, t == NT_INT ? "((double)%s)" : "%s", e); }

/* From here on, slot holds the declaration `key` (a slot is reused by
 * variables of blocks that have ended: each has a type of its own). */
static void declare(Gen *g, void *key, int slot) {
    int i = 0;
    while (i < g->ndecls && g->dkeys[i] != key) i++;
    if (i == g->ndecls) {
        if (g->ndecls == g->capdecls) {
            g->capdecls = g->capdecls ? g->capdecls * 2 : 16;
            g->dkeys = xrealloc(g->dkeys, sizeof(void *) * (size_t)g->capdecls);
            g->dtypes = xrealloc(g->dtypes, sizeof(int) * (size_t)g->capdecls);
            g->dshape = xrealloc(g->dshape, sizeof(int) * (size_t)g->capdecls);
            g->dneg = xrealloc(g->dneg, (size_t)g->capdecls);
        }
        g->dkeys[i] = key;
        g->dtypes[i] = 0;
        g->dshape[i] = -1;
        g->dneg[i] = 0;  /* (none yet: see nonneg) */
        g->ndecls++;
    }
    g->live[slot] = i;
}

/* The type slot's variable holds (0: none yet). */
static int type_of(Gen *g, int slot) { return g->live[slot] >= 0 ? g->dtypes[g->live[slot]] : 0; }

/* Is whole-number C expression e (a literal, a slot, a temporary, a
 * length) known not to be negative? A slot is while every value it gets is
 * not: an inductive guess, dropped (and the fn translated again) when one
 * might be (mark_neg). */
static int nonneg(Gen *g, const char *e) {
    if (e[0] == '(' && e[1] >= '0' && e[1] <= '9') return 1;  /* (5LL) */
    if (e[0] == 'k' && e[1] >= '0' && e[1] <= '9' && !strchr(e, ' ')) return 1;  /* (a position: checked, or counted from 0) */
    if ((e[0] == 'i' || e[0] == 't') && e[1] >= '0' && e[1] <= '9') {
        char *end;
        long k = strtol(e + 1, &end, 10);
        if (*end) return 0;
        return e[0] == 't' ? k < g->tmp && g->tnn[k] : g->live[k] >= 0 && type_of(g, (int)k) == NT_INT && !g->dneg[g->live[k]];
    }
    return (e[0] == 'L' && strstr(e, ".n") && !strchr(e, ' ')) || (e[0] == 'R' && e[1] == 'n');  /* (lengths) */
}

static void mark_neg(Gen *g, int slot) {
    int d = g->live[slot];
    if (d >= 0 && !g->dneg[d]) {
        g->dneg[d] = 1;
        g->widened = 1;  /* (what relied on it: translated again) */
    }
}

/* In a loop: slot is a global whose kind is not known yet (UNTYPED), now
 * used as a Number or Bool: it is one from here on. Returns its type, or 0
 * when slot is not such a global. */
static int loop_scalar(Gen *g, int slot, int t) {
    if (!g->cur->loop || slot >= g->cur->d->nparams || type_of(g, slot) != UNTYPED || g->cur->hint[slot] >= NT_LNUM) return 0;
    t = t == NT_BOOL ? NT_BOOL : NT_NUM;
    g->dtypes[g->live[slot]] = g->cur->params[slot] = t;
    g->widened = 1;
    return t;
}

/* The shape sh is now part of (the one whose fields every record of it has). */
static int shape_root(Gen *g, int sh) {
    while (g->shapes[sh].parent != sh) sh = g->shapes[sh].parent;
    return sh;
}
#define SHAPE(g, sh) (&(g)->shapes[shape_root(g, sh)])

/* Shapes a and b become one, with the fields of both (0 if too many). */
static int shape_join(Gen *g, int a, int b) {
    int ra = shape_root(g, a), rb = shape_root(g, b);
    if (ra == rb) return 1;
    for (int f = 0; f < g->shapes[rb].n; f++) {
        int e = 0;
        while (e < g->shapes[ra].n && g->shapes[ra].fields[e] != g->shapes[rb].fields[f]) e++;
        if (e < g->shapes[ra].n) continue;
        if (g->shapes[ra].n == LUBE_REC_MAX) return fail(g);
        g->shapes[ra].fields[g->shapes[ra].n++] = g->shapes[rb].fields[f];
    }
    g->shapes[rb].parent = ra;
    g->widened = 1;
    g->shape_changes++;
    return 1;
}

/* n is a record parameter, or a record taken out of one, of kind `want`
 * (NT_RLIST or NT_REC): its slot, + 1; else 0. An untyped parameter takes
 * the kind of its first use. */
static int record(Gen *g, Node *n, int want) {
    if (n->kind != N_VAR || n->as.var.kind != VAR_LOCAL || g->live[n->as.var.index] < 0) return 0;
    int d = g->live[n->as.var.index], *t = &g->dtypes[d];
    if (*t == UNTYPED && g->cur->loop && g->cur->hint[n->as.var.index] >= NT_LNUM) return 0;  /* (a loop's list) */
    if (*t == UNTYPED) {  /* (its parameter now has a kind: the fn's signature changes) */
        *t = want;
        g->cur->params[n->as.var.index] = want;
        g->widened = 1;
    }
    return *t == want && g->dshape[d] >= 0 ? n->as.var.index + 1 : 0;
}

/* Where field `name` is in the record in slot s (added to its shape when new). */
static int field_of(Gen *g, int s, ObjString *name) {
    int sh = shape_root(g, g->dshape[g->live[s]]), f = 0;
    while (f < g->shapes[sh].n && g->shapes[sh].fields[f] != name) f++;
    if (f == g->shapes[sh].n) {
        if (f == LUBE_REC_MAX) return fail(g) - 1;
        g->shapes[sh].fields[g->shapes[sh].n++] = name;
        g->widened = 1;  /* (the rows are wider: translate again) */
        g->shape_changes++;
    }
    return f;
}

/* xs[i], xs a record list: the C for its row's first number (0 if not). */
static int row_place(Gen *g, Node *n, char *place) {
    int s = record(g, n->as.index.object, NT_RLIST) - 1;
    char i[64];
    int ti = s >= 0 ? gen_expr(g, n->as.index.index, i) : 0, k = new_tmp(g);
    if (ti == NT_INT)
        sb_printf(g->out, "int64_t k%d = %s;\nif (k%d < 0) k%d += Rn%d;\nif (k%d < 0 || k%d >= Rn%d) %s\n", k, i, k, k, s, k,
                  k, s, bail(g));
    else if (ti == NT_NUM)
        sb_printf(g->out, "int64_t k%d;\nif (lube_nidx(Rn%d, %s, &k%d)) %s\n", k, s, i, k, bail(g));
    else
        return fail(g);
    snprintf(place, 64, "R%d + k%d * %d", s, k, SHAPE(g, g->dshape[g->live[s]])->n);
    return 1;
}

/* Slot s (just declared: let r = xs[i], or a loop's) becomes a copy of the
 * record at C address `from`, of shape sh. (Nothing else gives that
 * declaration a type: it holds this record kind only.) */
static void record_copy(Gen *g, int s, int sh, const char *from) {
    int d = g->live[s];
    g->dtypes[d] = NT_REC;
    g->dshape[d] = sh;
    sb_printf(g->out, "memcpy(Bs%d, %s, sizeof(double) * %d);\nBp%d = Bs%d;\n", s, from, SHAPE(g, sh)->n, s, s);
}

/* The C for a record's field (xs[i].f, r.f) read or written, in place; 0 if
 * it is not one. */
static int field_place(Gen *g, Node *n, char *place) {
    Node *obj = n->as.field.object;
    int s, f;
    if (obj->kind == N_INDEX && (s = record(g, obj->as.index.object, NT_RLIST))) {
        char row[64];
        if (!row_place(g, obj, row) || (f = field_of(g, s - 1, n->as.field.name)) < 0) return 0;
        snprintf(place, 64, "(%s)[%d]", row, f);
        return 1;
    }
    if (!(s = record(g, obj, NT_REC))) return fail(g);
    s--;
    if ((f = field_of(g, s, n->as.field.name)) < 0) return 0;
    snprintf(place, 64, "Bp%d[%d]", s, f);
    return 1;
}

/* A value of type t goes into slot: its variable's type joins t (a whole
 * number in a variable that also holds other Numbers is a double); sets
 * *how to the value converted to that type. 0 if they cannot share it. */
static int slot_store(Gen *g, int slot, int t, const char *v, char *how) {
    if (g->live[slot] < 0) return fail(g);  /* (a slot assigned with no declaration: not one of the fn's) */
    int *pt = &g->dtypes[g->live[slot]], st = *pt;
    if (st < 0 || st == NT_VAL || t == NT_VAL) return fail(g);  /* (a record parameter given another value; values: in parameters only) */
    if (!st) st = *pt = t;
    if (st >= NT_LNUM || t >= NT_LNUM) return st == t ? st : fail(g);  /* (handled by the caller) */
    if (st == NT_INT && t == NT_NUM) {
        st = *pt = NT_NUM;
        g->widened = 1;
    }
    if ((st == NT_BOOL) != (t == NT_BOOL)) return fail(g);
    if (st == NT_NUM) as_double(t, v, how);
    else snprintf(how, 64, "%s", v);
    return st;
}

static void store(Gen *g, int slot, int t, const char *v) {
    char how[64];
    int st = slot_store(g, slot, t, v, how);
    if (st == NT_INT && !nonneg(g, how)) mark_neg(g, slot);
    if (st) sb_printf(g->out, "%c%d = %s;\n", letter(st), slot, how);
}

/* ---------- expressions: each leaves its value in a C expression (res) ---------- */


static void number_literal(char *res, double x) {  /* (in parentheses: -x of -2.5 is not --2.5) */
    snprintf(res, 64, "(%.17g", x);
    if (!strpbrk(res, ".en")) strcat(res, ".0");  /* a double, never an int (1/2 is not 0) */
    strcat(res, ")");
}

/* A value of this fn's own, released on its way out unless given away (see NT_VAL). */
static int new_vt(Gen *g) { return g->nvt++; }

/* Done with value e: released, when it is this fn's own. */
static void done_with(Gen *g, const char *e) {
    if (!strncmp(e, "vt", 2)) sb_printf(g->out, "release(%s);\n%s = NIL_VAL;\n", e, e);
}

/* Value e (of type *t, NT_VAL) as a Number or a Bool (want), or the VM
 * runs the call: *t and e become those. */
static void unbox(Gen *g, int *t, char *e, int want) {
    if (*t != NT_VAL) return;
    int k = new_tmp(g);
    if (want == NT_BOOL) sb_printf(g->out, "int t%d;\nif (lube_nv_bool(%s, &t%d)) %s\n", k, e, k, bail(g));
    else sb_printf(g->out, "double t%d;\nif (lube_nv_num(%s, &t%d)) %s\n", k, e, k, bail(g));
    done_with(g, e);
    snprintf(e, 64, "t%d", k);
    *t = want == NT_BOOL ? NT_BOOL : NT_NUM;
}

/* Number, Bool or value e (of type t) as a value: res is e's own when it was. */
static int boxed(int t, const char *e, char *res) {
    if (t == NT_VAL) snprintf(res, 64, "%s", e);
    else if (NUMERIC(t)) snprintf(res, 64, t == NT_INT ? "NUM_VAL((double)%s)" : "NUM_VAL(%s)", e);
    else if (t == NT_BOOL) snprintf(res, 64, "BOOL_VAL(%s)", e);
    else return 0;
    return 1;
}

/* a op b (of types ta, tb, both Numbers or both Bools); returns the result's type. */
static int gen_binop(Gen *g, TokenType op, int ta, const char *a, int tb, const char *b, char *res) {
    static const char *const cmp[] = {"<", "<=", ">", ">=", "==", "!="};
    int c = op == T_LT ? 0 : op == T_LE ? 1 : op == T_GT ? 2 : op == T_GE ? 3 : op == T_EQ ? 4 : op == T_NEQ ? 5 : -1;
    int t = new_tmp(g);
    if (ta == NT_BOOL || tb == NT_BOOL) {
        if (ta != tb || c < 4) return fail(g);
        sb_printf(g->out, "int t%d = %s %s %s;\n", t, a, cmp[c], b);
    } else if (ta == NT_INT && tb == NT_INT && c >= 0) {
        sb_printf(g->out, "int t%d = %s %s %s;\n", t, a, cmp[c], b);
    } else if (ta == NT_INT && tb == NT_INT && (op == T_PLUS || op == T_MINUS)) {  /* exact while within 2^53 */
        if (g->fast) sb_printf(g->out, "int64_t t%d = (int64_t)((uint64_t)%s %c (uint64_t)%s);\n", t, a, op == T_PLUS ? '+' : '-', b);
        else sb_printf(g->out, "int64_t t%d = %s %c %s;\nif (LUBE_OUT(t%d)) %s\n", t, a, op == T_PLUS ? '+' : '-', b, t, bail(g));
        g->tnn[t] = op == T_PLUS && nonneg(g, a) && nonneg(g, b);
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_STAR && g->fast) {  /* (wrapping; still never -0) */
        sb_printf(g->out, "int64_t t%d = (int64_t)((uint64_t)%s * (uint64_t)%s);\n", t, a, b);
        sb_printf(g->out, "if (t%d == 0 && (%s < 0 || %s < 0)) %s\n", t, a, b, bail(g));
        g->tnn[t] = nonneg(g, a) && nonneg(g, b);
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_STAR) {  /* (as a double: exact when it fits; never -0) */
        sb_printf(g->out, "double m%d = (double)%s * (double)%s;\n", t, a, b);
        sb_printf(g->out, "if (m%d > LUBE_BIGD || m%d < -LUBE_BIGD || (m%d == 0 && (%s < 0 || %s < 0))) %s\n", t, t,
                  t, a, b, bail(g));
        sb_printf(g->out, "int64_t t%d = (int64_t)m%d;\n", t, t);
        g->tnn[t] = nonneg(g, a) && nonneg(g, b);
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_PERCENT && nonneg(g, a) && b[0] == '(' && b[1] >= '1' && b[1] <= '9') {
        sb_printf(g->out, "int64_t t%d = %s %% %s;\n", t, a, b);  /* (neither negative: C's %, never -0) */
        g->tnn[t] = 1;
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_PERCENT) {  /* Lube's %: the sign of b; never -0 */
        sb_printf(g->out, g->fast ? "if (%s == 0) %s\nint64_t t%d = %s == -1 ? 0 : %s %% %s;\n"  /* (wrapped: INT64_MIN % -1) */
                                  : "if (%s == 0) %s\nint64_t t%d = %s%s %% %s;\n", b, bail(g), t, g->fast ? b : "", a, b);
        sb_printf(g->out, "if (t%d != 0 && (t%d < 0) != (%s < 0)) t%d += %s;\nelse if (t%d == 0 && %s < 0) %s\n", t, t,
                  b, t, b, t, a, bail(g));
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else {  /* doubles, as the VM computes */
        char x[64], y[64];
        as_double(ta, a, x);
        as_double(tb, b, y);
        if (c >= 0) {
            sb_printf(g->out, "int t%d = %s %s %s;\n", t, x, cmp[c], y);
        } else if (op == T_PLUS || op == T_MINUS || op == T_STAR) {
            sb_printf(g->out, "double t%d = %s %c %s;\n", t, x, op == T_PLUS ? '+' : op == T_MINUS ? '-' : '*', y);
            snprintf(res, 64, "t%d", t);
            return NT_NUM;
        } else if (op == T_SLASH || op == T_PERCENT) {  /* (by zero: the VM raises the error) */
            sb_printf(g->out, "if (%s == 0) %s\ndouble t%d = ", y, bail(g), t);
            sb_printf(g->out, op == T_SLASH ? "%s / %s;\n" : "lube_nmod(%s, %s);\n", x, y);
            snprintf(res, 64, "t%d", t);
            return NT_NUM;
        } else if (op == T_POW) {
            sb_printf(g->out, "double t%d = lube_m_pow(%s, %s);\n", t, x, y);
            snprintf(res, 64, "t%d", t);
            return NT_NUM;
        } else {
            return fail(g);
        }
    }
    snprintf(res, 64, "t%d", t);
    return NT_BOOL;
}

/* n is a local list of the fn's own (or a loop's global list, of the kind
 * it was declared as): its type (NT_LNUM, NT_LBOOL...); else 0. */
static int list_slot(Gen *g, Node *n) {
    int s = n->kind == N_VAR && n->as.var.kind == VAR_LOCAL ? n->as.var.index : -1, t = s >= 0 ? type_of(g, s) : 0;
    if (t == UNTYPED && g->cur->loop && s < g->cur->d->nparams && g->cur->hint[s] >= NT_LNUM) {
        t = g->dtypes[g->live[s]] = g->cur->params[s] = g->cur->hint[s];
        g->widened = 1;
    }
    return t >= NT_LNUM && t <= NT_TLBOOL ? t : 0;
}

/* The position index (an expression) names in list slot s, as the VM finds
 * it: whole, from the end when negative, in range (else the VM's error).
 * Returns n + 1 for the temporary k<n> holding it (0 if it cannot be native). */
static int gen_index(Gen *g, int s, int lt, Node *index) {
    char i[64];
    int ti = gen_expr(g, index, i), k = new_tmp(g);
    char L = LCHAR(lt);
    if (ti == NT_INT)
        sb_printf(g->out, "int64_t k%d = %s;\nif (k%d < 0) k%d += L%c%d.n;\nif (k%d < 0 || k%d >= L%c%d.n) %s\n", k, i, k,
                  k, L, s, k, k, L, s, bail(g));
    else if (ti == NT_NUM)
        sb_printf(g->out, "int64_t k%d;\nif (lube_nidx(L%c%d.n, %s, &k%d)) %s\n", k, L, s, i, k, bail(g));
    else
        return fail(g);
    return k + 1;
}

/* A list literal's items ([a, b], Number[], Bool[a]) pushed into the
 * scratch list (Lnt or Lbt), emptied first; expect: the type it must be
 * (0: either). Returns its type, or 0. */
static int gen_items(Gen *g, Node *n, int expect) {
    NodeList *items = n->kind == N_LIST ? &n->as.items : n->kind == N_TYPED_LIST ? &n->as.tlist.items : NULL;
    int typed = n->kind == N_TYPED_LIST;
    int lt = !items ? -1 : !typed ? 0 : n->as.tlist.elem == TT_NUMBER ? NT_TLNUM : n->as.tlist.elem == TT_BOOL ? NT_TLBOOL : -1;
    if (lt < 0 || (expect && lt && lt != expect)) return fail(g);
    if (!typed && expect && expect != NT_LNUM && expect != NT_LBOOL) return fail(g);  /* (an untyped list into a typed one's var) */
    if (!lt) lt = expect;
    if (lt) sb_printf(g->out, "L%ct.n = 0;\n", LCHAR(lt));
    for (int i = 0; i < items->count; i++) {
        char v[64], d[64];
        int t = gen_expr(g, items->items[i], v);
        int it = NUMERIC(t) ? (typed ? NT_TLNUM : NT_LNUM) : t == NT_BOOL ? (typed ? NT_TLBOOL : NT_LBOOL) : 0;
        if (!it || (lt && it != lt)) return fail(g);
        if (!lt) sb_printf(g->out, "L%ct.n = 0;\n", LCHAR(it));  /* (the first item decides) */
        lt = it;
        if (LIST_NUM(lt)) as_double(t, v, d);
        else snprintf(d, sizeof d, "%s", v);
        sb_printf(g->out, "if (%s_push(&L%ct, %s)) %s\n", LTYPE(lt), LCHAR(lt), d, bail(g));
    }
    return lt ? lt : fail(g);  /* ([] alone: a list of what?) */
}

/* A list made into list slot s, its old items let go: a literal, or a
 * literal repeated ([x] * n, n * [x]). Returns its type, or 0. */
static int gen_list_into(Gen *g, int s, Node *n) {
    Node *lit = n, *count = NULL;
    if (n->kind == N_BINARY && n->as.binary.op == T_STAR) {
        int left = n->as.binary.left->kind == N_LIST || n->as.binary.left->kind == N_TYPED_LIST;
        lit = left ? n->as.binary.left : n->as.binary.right;
        count = left ? n->as.binary.right : n->as.binary.left;
    }
    int st = type_of(g, s), lt = gen_items(g, lit, st >= NT_LNUM ? st : 0);
    if (!lt || (st && st != lt) || g->live[s] < 0) return fail(g);
    char L = LCHAR(lt), times[64], v[64];
    const char *T = LTYPE(lt);
    if (count) {  /* (repeat lets go of what was in slot s) */
        int tc = gen_expr(g, count, times);
        if (!NUMERIC(tc)) return fail(g);
        as_double(tc, times, v);
        sb_printf(g->out, "if (%s_repeat(&L%c%d, &L%ct, %s)) %s\n", T, L, s, L, v, bail(g));
    } else {  /* the scratch list's array moves into slot s */
        sb_printf(g->out, "free(L%c%d.a);\nL%c%d = L%ct;\nL%ct.a = 0;\nL%ct.n = L%ct.cap = 0;\n", L, s, L, s, L, L, L, L);
    }
    g->dtypes[g->live[s]] = lt;
    return lt;
}

/* xs.push(v) or push(&xs, v), xs a list of the fn's own. */
static void gen_push(Gen *g, Node *list, Node *value) {
    char v[64], d[64];
    int lt = list_slot(g, list), t = lt ? gen_expr(g, value, v) : 0;
    unbox(g, &t, v, LIST_NUM(lt) ? NT_NUM : NT_BOOL);
    if (!lt || (LIST_NUM(lt) ? !NUMERIC(t) : t != NT_BOOL)) {
        fail(g);
        return;
    }
    if (LIST_NUM(lt)) as_double(t, v, d);
    else snprintf(d, sizeof d, "%s", v);
    sb_printf(g->out, "if (%s_push(&L%c%d, %s)) %s\n", LTYPE(lt), LCHAR(lt), list->as.var.index, d, bail(g));
}

/* The built-in `name` (a global no file of the program declares): for len and push. */
static int is_builtin(Gen *g, Node *callee, const char *name) {
    return callee->kind == N_VAR && callee->as.var.kind == VAR_GLOBAL && !strcmp(callee->as.var.name->chars, name) &&
           !map_get(g->declared, OBJ_VAL(callee->as.var.name), NULL);
}

static int is_list_expr(Node *n) {
    if (n->kind == N_BINARY && n->as.binary.op == T_STAR) return is_list_expr(n->as.binary.left) || is_list_expr(n->as.binary.right);
    return n->kind == N_LIST || n->kind == N_TYPED_LIST;
}

/* A math built-in: its C, or NULL. */
static const char *builtin(ObjString *name, int nargs) {
    /* (those not exact to the last bit call Lube's: see lube_m_pow) */
    static const char *const one[][2] = {{"sqrt", "sqrt"}, {"sin", "lube_m_sin"}, {"cos", "lube_m_cos"},
                                         {"tan", "lube_m_tan"}, {"exp", "lube_m_exp"}, {"floor", "floor"},
                                         {"ceil", "ceil"}, {"abs", "fabs"}, {"log", "lube_m_log"},
                                         {"round", "lube_nround"}};
    static const char *const two[][2] = {{"pow", "lube_m_pow"}, {"atan2", "lube_m_atan2"}, {"log", "lube_nlog2"}};
    for (size_t i = 0; nargs == 1 && i < sizeof one / sizeof one[0]; i++)
        if (!strcmp(name->chars, one[i][0])) return one[i][1];
    for (size_t i = 0; nargs == 2 && i < sizeof two / sizeof two[0]; i++)
        if (!strcmp(name->chars, two[i][0])) return two[i][1];
    return NULL;
}

/* The root variable of a place (x of x, x[i], x.f, x[i].f), or NULL. */
static Node *place_var(Node *x) {
    while (x && (x->kind == N_INDEX || x->kind == N_FIELD)) x = x->kind == N_INDEX ? x->as.index.object : x->as.field.object;
    return x && x->kind == N_VAR && x->as.var.kind == VAR_LOCAL ? x : NULL;
}

/* x, given for c's record parameter p: a record (or list of them) of this
 * fn's, or a row of one (xs[i]), passed as it is: its C (a pointer, and a
 * list's length) in res, laid out as p is (their shapes join). Sets *slot to
 * the variable it is in. Returns its kind, or 0. */
static int gen_record_arg(Gen *g, Node *x, Cand *c, int p, char *res, int *slot) {
    Node *v = x->kind == N_INDEX ? x->as.index.object : x;
    int s = v->kind == N_VAR && v->as.var.kind == VAR_LOCAL ? v->as.var.index : -1;
    int st = s >= 0 ? type_of(g, s) : 0, want = c->params[p];
    if (s < 0 || !(st == UNTYPED || RECORD(st))) return fail(g);
    if (x->kind == N_INDEX) {  /* a row: a record */
        if (want == UNTYPED) want = c->params[p] = NT_REC, g->shape_changes++;  /* (the callee learns its kind) */
        if (want != NT_REC || !record(g, v, NT_RLIST) || !row_place(g, x, res)) return fail(g);
    } else {
        if (want == UNTYPED && st == UNTYPED) return fail(g);  /* (neither says which kind) */
        if (want == UNTYPED) want = c->params[p] = st, g->shape_changes++;
        if (!record(g, v, want)) return fail(g);
        if (want == NT_REC) snprintf(res, 64, "Bp%d", s);
        else snprintf(res, 64, "R%d, Rn%d", s, s);
    }
    *slot = s;
    if (c->cls[p] && g->cur->cls[s] != c->cls[p]) {  /* the callee calls methods on it: so it is of that class */
        if (s >= g->cur->d->nparams || g->cur->cls[s]) return fail(g);  /* (a copy's class is not known) */
        g->cur->cls[s] = c->cls[p];  /* (its wrapper checks it) */
        g->shape_changes++;
    }
    return shape_join(g, g->dshape[g->live[s]], c->shape[p]) ? want : 0;
}

/* A call; need: its value is used. */
static int gen_call(Gen *g, Node *n, char *res, int need) {
    Node *callee = n->as.call.callee;
    NodeList *args = &n->as.call.args;
    if (callee->kind != N_VAR || args->count > NATIVE_MAX_PARAMS) return fail(g);
    if (is_builtin(g, callee, "len") && args->count == 1 && list_slot(g, args->items[0])) {
        int lt = list_slot(g, args->items[0]);
        snprintf(res, 64, "L%c%d.n", LCHAR(lt), args->items[0]->as.var.index);
        return NT_INT;
    }
    int rs = is_builtin(g, callee, "len") && args->count == 1 ? record(g, args->items[0], NT_RLIST) : 0;
    if (rs) {
        snprintf(res, 64, "Rn%d", rs - 1);
        return NT_INT;
    }
    if (is_builtin(g, callee, "len") && args->count == 1 && is_value(g, args->items[0])) {  /* a list value's length */
        char v[64];
        gen_expr(g, args->items[0], v);
        int k = new_tmp(g);
        sb_printf(g->out, "int64_t t%d;\nif (lube_nv_len(%s, &t%d)) %s\n", k, v, k, bail(g));
        g->tnn[k] = 1;
        snprintf(res, 64, "t%d", k);
        return NT_INT;
    }
    if (is_builtin(g, callee, "push") && args->count == 2 && args->items[0]->kind == N_REF && !need) {
        gen_push(g, args->items[0]->as.expr, args->items[1]);
        return g->failed ? 0 : NT_NUM;
    }
    VarRef *v = &callee->as.var;
    Cand *c = v->kind == VAR_SELF ? g->cur : v->kind == VAR_GLOBAL ? cand_for(g, v->name) : NULL;
    const char *fn = !c && v->kind == VAR_GLOBAL && !map_get(g->declared, OBJ_VAL(v->name), NULL)
                         ? builtin(v->name, args->count) : NULL;
    if (!c && !fn) return fail(g);
    return gen_invoke(g, c, fn, args->items, args->count, 0, res, need);
}

/* Is c's parameter p lent to it (&, or a method's `this`)? */
static int lent_param(Cand *c, int p) { return c->d->modes[p] == PARAM_REF || (c->method && p == 0); }

/* Calls candidate c (or the math built-in fn) with the argc arguments in
 * argv; self: argv[0] is a method's receiver. need: its value is used. */
static int gen_invoke(Gen *g, Cand *c, const char *fn, Node **argv, int argc, int self, char *res, int need) {
    char a[NATIVE_MAX_PARAMS][64], x[2][64];
    int ta[NATIVE_MAX_PARAMS];
    int whole = 1;  /* every Number argument is whole: the callee's whole-number version can take them */
    int lent = 0, nrec = 0, rslots[NATIVE_MAX_PARAMS];
    for (int i = 0; i < argc; i++) {
        int want = !c ? NT_NUM : i < c->d->nparams ? c->params[i] : -1;
        if (c && i < c->d->nparams && (want == UNTYPED || RECORD(want))) {  /* a record: passed as it is */
            int ref = lent_param(c, i), recv = self && i == 0;
            Node *x = !ref || recv ? argv[i] : argv[i]->kind == N_REF ? argv[i]->as.expr : NULL;
            if (!x || (c->d->modes[i] == PARAM_VAR && !recv)) return fail(g);  /* (var: the callee's own copy, from the VM) */
            if (!(ta[i] = gen_record_arg(g, x, c, i, a[i], &rslots[nrec++]))) return 0;
            lent |= ref;
            continue;
        }
        ta[i] = gen_expr(g, argv[i], a[i]);
        if (want == NT_VAL) {  /* a value: as it is (borrowed by the callee), or a Number or Bool boxed */
            char bx[64];
            if (!boxed(ta[i], a[i], bx)) return fail(g);
            snprintf(a[i], 64, "%s", bx);
            ta[i] = NT_VAL;
            continue;
        }
        unbox(g, &ta[i], a[i], want);
        if (want < 0 || !ta[i] || (want == NT_NUM ? !NUMERIC(ta[i]) : ta[i] != want)) return fail(g);
        whole &= ta[i] != NT_NUM;
    }
    for (int i = 0; lent && i < nrec; i++)  /* (one changed through & is not also another argument) */
        for (int j = i + 1; j < nrec; j++)
            if (rslots[i] == rslots[j]) return fail(g);
    int t = new_tmp(g);
    if (fn) {  /* (a whole number's floor, ceil and round are itself, and its abs is whole) */
        if (ta[0] == NT_INT && (!strcmp(fn, "floor") || !strcmp(fn, "ceil") || !strcmp(fn, "lube_nround"))) {
            snprintf(res, 64, "%s", a[0]);
            return NT_INT;
        }
        if (ta[0] == NT_INT && !strcmp(fn, "fabs")) {
            sb_printf(g->out, "int64_t t%d = %s < 0 ? -%s : %s;\n", t, a[0], a[0], a[0]);
            snprintf(res, 64, "t%d", t);
            return NT_INT;
        }
        as_double(ta[0], a[0], x[0]);
        if (argc > 1) as_double(ta[1], a[1], x[1]);
        sb_printf(g->out, "double t%d = %s(%s%s%s);\n", t, fn, x[0], argc > 1 ? ", " : "", argc > 1 ? x[1] : "");
        snprintf(res, 64, "t%d", t);
        return NT_NUM;
    }
    if (argc != c->d->nparams || (need && c->ret == NT_NONE)) return fail(g);
    int k = (int)(c - g->cands);
    if (whole && c->ok_int) {  /* its whole-number version */
        sb_printf(g->out, "int64_t t%d = ni_%d(", t, k);
        for (int i = 0; i < argc; i++) sb_printf(g->out, "%s, ", a[i]);
        sb_printf(g->out, "left - 1);\nif (t%d == LUBE_FAIL) %s\n", t, bail(g));
        snprintf(res, 64, c->ret == NT_BOOL ? "((int)t%d)" : "t%d", t);
        return c->ret == NT_BOOL ? NT_BOOL : NT_INT;
    }
    int vt = c->ret == NT_VAL ? new_vt(g) : -1;  /* (a value it returns: this fn's own) */
    if (vt < 0) sb_printf(g->out, "double t%d;\n", t);
    sb_printf(g->out, "if (nf_%d(", k);
    for (int i = 0; i < argc; i++) {
        if (RECORD(ta[i]) || ta[i] == NT_VAL) {
            sb_printf(g->out, "%s, ", a[i]);
            continue;
        }
        as_double(ta[i], a[i], x[0]);
        sb_printf(g->out, "%s, ", x[0]);
    }
    if (vt >= 0) sb_printf(g->out, "&vt%d, left - 1)) %s\n", vt, bail(g));
    else sb_printf(g->out, "&t%d, left - 1)) %s\n", t, bail(g));
    for (int i = 0; i < argc; i++)
        if (ta[i] == NT_VAL) done_with(g, a[i]);  /* (values it made for the call) */
    if (vt >= 0) {
        snprintf(res, 64, "vt%d", vt);
        return NT_VAL;
    }
    snprintf(res, 64, c->ret == NT_BOOL ? "(t%d != 0)" : "t%d", t);
    return c->ret == NT_BOOL ? NT_BOOL : NT_NUM;
}

/* Is n a borrowed value: a value parameter, or an item of one (v[i][j])? */
static int is_value(Gen *g, Node *n) {
    if (n->kind == N_INDEX) return is_value(g, n->as.index.object);
    return n->kind == N_VAR && n->as.var.kind == VAR_LOCAL && type_of(g, n->as.var.index) == NT_VAL;
}

/* Is a method named `name` one of class k's own? */
static FuncDecl *own_method(Gen *g, int k, ObjString *name) {
    NodeList *ms = &g->classes[k].node->as.klass.methods;
    for (int i = 0; i < ms->count; i++)
        if (ms->items[i]->as.func->name == name) return ms->items[i]->as.func;
    return NULL;
}

/* The method `name` of the records in slot s: those of a parameter (a list's: its records), of
 * one class (the wrapper checks it): the class it already has, or the one class with such a
 * method. Only the class's own methods (a parent's could call one the class overrides). */
static Cand *receiver_method(Gen *g, int s, ObjString *name) {
    Cand *c = g->cur;
    if (s >= c->d->nparams) return NULL;  /* (a copy: its class is not known) */
    int k = c->cls[s] - 1;
    for (int i = 0; k < 0 && i < g->nclasses; i++)
        if (own_method(g, i, name)) {
            for (int j = i + 1; j < g->nclasses; j++)
                if (own_method(g, j, name)) return NULL;  /* (two classes have one) */
            k = i;
            c->cls[s] = k + 1;
            g->shape_changes++;  /* (its callers learn it too) */
        }
    FuncDecl *d = k >= 0 ? own_method(g, k, name) : NULL;
    for (int i = 0; d && i < g->ncands; i++)
        if (g->cands[i].d == d && g->cands[i].ok) return &g->cands[i];
    return NULL;
}

/* May this fn change the record (or list) in slot s, as a method changing `this` would? */
static int changeable(Gen *g, int s) {
    Cand *c = g->cur;
    if (s >= c->d->nparams) return 0;  /* (copies: let r = xs[i], a loop's) */
    if (c->loop) return !c->fixed[s];
    return (c->method && s == 0) || c->d->modes[s] != PARAM_LET;
}

/* x.name(args), x a record (or a row of a list of them) whose class has that method, native. */
static int gen_method(Gen *g, Node *n, char *res, int need) {
    Node *obj = n->as.method.object, *argv[NATIVE_MAX_PARAMS];
    NodeList *args = &n->as.method.args;
    Node *v = obj->kind == N_INDEX ? obj->as.index.object : obj;
    if (args->count >= NATIVE_MAX_PARAMS || v->kind != N_VAR || v->as.var.kind != VAR_LOCAL) return fail(g);
    Cand *m = receiver_method(g, v->as.var.index, n->as.method.name);
    if (!m || (writes_to(m->d->body, 0) && !changeable(g, v->as.var.index))) return fail(g);  /* (the VM's error) */
    argv[0] = obj;
    for (int i = 0; i < args->count; i++) argv[i + 1] = args->items[i];
    return gen_invoke(g, m, NULL, argv, args->count + 1, 1, res, need);
}

/* n, where a Bool is wanted (a value unboxed). */
static int bool_expr(Gen *g, Node *n, char *res) {
    int t = gen_expr(g, n, res);
    unbox(g, &t, res, NT_BOOL);
    return t;
}

static int gen_expr(Gen *g, Node *n, char *res) {
    char a[64], b[64];
    if (!g->failed) g->line = n->line;
    switch (n->kind) {
    case N_LITERAL:
        if (IS_NUM(n->as.literal)) {
            double x = n->as.literal.as.number;
            if (x >= -9007199254740992.0 && x <= 9007199254740992.0 && x == (double)(int64_t)x && !(x == 0 && signbit(x))) {
                snprintf(res, 64, "(%lldLL)", (long long)x);  /* a whole number */
                return NT_INT;
            }
            number_literal(res, x);
            return NT_NUM;
        }
        if (!IS_BOOL(n->as.literal)) return fail(g);
        strcpy(res, n->as.literal.as.boolean ? "1" : "0");
        return NT_BOOL;
    case N_VAR: {
        int t = n->as.var.kind == VAR_LOCAL ? type_of(g, n->as.var.index) : 0;
        if (t == NT_VAL) {  /* a parameter's value, or a variable's (borrowed) */
            snprintf(res, 64, "%c%d", n->as.var.index < g->cur->d->nparams ? 'p' : 'V', n->as.var.index);
            return NT_VAL;
        }
        if (t == UNTYPED && g->cur->loop) t = loop_scalar(g, n->as.var.index, g->cur->hint[n->as.var.index]);
        if (t <= 0 || t >= NT_LNUM) return fail(g);  /* (a list or record is not a value here: it never leaves the fn) */
        snprintf(res, 64, "%c%d", letter(t), n->as.var.index);
        return t;
    }
    case N_UNARY: {
        int t = gen_expr(g, n->as.unary.operand, a);
        unbox(g, &t, a, n->as.unary.op != T_MINUS ? NT_BOOL : NT_NUM);
        int k = new_tmp(g);
        if (n->as.unary.op != T_MINUS) {  /* not */
            if (t != NT_BOOL) return fail(g);
            sb_printf(g->out, "int t%d = !%s;\n", k, a);
        } else if (t == NT_INT && g->mode) {  /* (-0 is a double's: the double version computes it) */
            sb_printf(g->out, "if (%s == 0) %s\nint64_t t%d = -%s;\n", a, bail(g), k, a);
        } else if (NUMERIC(t)) {
            as_double(t, a, b);
            sb_printf(g->out, "double t%d = -%s;\n", k, b);
            t = NT_NUM;
        } else {
            return fail(g);
        }
        snprintf(res, 64, "t%d", k);
        return n->as.unary.op != T_MINUS ? NT_BOOL : t;
    }
    case N_BINARY: {
        int ta = gen_expr(g, n->as.binary.left, a);
        int tb = ta ? gen_expr(g, n->as.binary.right, b) : 0;  /* (values: unboxed as the other side is) */
        unbox(g, &ta, a, tb == NT_BOOL ? NT_BOOL : NT_NUM);
        unbox(g, &tb, b, ta == NT_BOOL ? NT_BOOL : NT_NUM);
        if (!ta || !tb) return fail(g);
        return gen_binop(g, n->as.binary.op, ta, a, tb, b, res);
    }
    case N_LIST: {  /* [a, b]: a new value (its items given to it) */
        NodeList *items = &n->as.items;
        char it[64], bx[64];
        int k = new_vt(g), owned[64], nowned = 0;
        StrBuf all;
        sb_init(&all);
        sb_putc(&all, ' ');
        for (int i = 0; i < items->count && !g->failed; i++) {
            int t = gen_expr(g, items->items[i], it);
            if (items->count > 64 || !boxed(t, it, bx)) {
                fail(g);
                break;
            }
            if (t == NT_VAL && !strncmp(it, "vt", 2)) owned[nowned++] = atoi(it + 2);
            if (t == NT_VAL && strncmp(it, "vt", 2)) sb_printf(&all, "retain(%s), ", bx);  /* (borrowed: one more owner) */
            else sb_printf(&all, "%s, ", bx);
        }
        if (!g->failed && items->count)
            sb_printf(g->out, "{\nValue it%d[] = {%s};\nvt%d = lube_nv_list(%d, it%d);\n}\n", k, all.data, k, items->count, k);
        else if (!g->failed)
            sb_printf(g->out, "vt%d = lube_nv_list(0, 0);\n", k);
        for (int i = 0; i < nowned; i++) sb_printf(g->out, "vt%d = NIL_VAL;\n", owned[i]);  /* (the list's now) */
        sb_free(&all);
        if (g->failed) return 0;
        snprintf(res, 64, "vt%d", k);
        return NT_VAL;
    }
    case N_AND:
    case N_OR: {  /* both Bools; the right one only runs when it decides */
        if (bool_expr(g, n->as.binary.left, a) != NT_BOOL) return fail(g);
        int k = new_tmp(g);
        sb_printf(g->out, "int t%d = %s;\nif (%st%d) {\n", k, a, n->kind == N_AND ? "" : "!", k);
        if (bool_expr(g, n->as.binary.right, b) != NT_BOOL) return fail(g);
        sb_printf(g->out, "t%d = %s;\n}\n", k, b);
        snprintf(res, 64, "t%d", k);
        return NT_BOOL;
    }
    case N_CALL:
        return gen_call(g, n, res, 1);
    case N_METHOD:
        return gen_method(g, n, res, 1);
    case N_FIELD: {  /* xs[i].x, r.x: a record's field */
        char p[64];
        if (!field_place(g, n, p)) return fail(g);
        int t = new_tmp(g);
        sb_printf(g->out, "double t%d = %s;\n", t, p);
        snprintf(res, 64, "t%d", t);
        return NT_NUM;
    }
    case N_INDEX: {  /* xs[i], xs a list of the fn's own */
        if (is_value(g, n->as.index.object)) {  /* v[i], v a value (borrowed): its item, borrowed too */
            char ix[64], dx[64];
            int to = gen_expr(g, n->as.index.object, a), ti = to ? gen_expr(g, n->as.index.index, ix) : 0;
            unbox(g, &ti, ix, NT_NUM);
            if (to != NT_VAL || !NUMERIC(ti)) return fail(g);
            int k = new_tmp(g);
            as_double(ti, ix, dx);
            sb_printf(g->out, "Value vb%d;\nif (lube_nv_index(%s, %s, &vb%d)) %s\n", k, a, dx, k, bail(g));
            snprintf(res, 64, "vb%d", k);
            return NT_VAL;
        }
        int lt = list_slot(g, n->as.index.object), k;
        if (!lt || !(k = gen_index(g, n->as.index.object->as.var.index, lt, n->as.index.index))) return fail(g);
        k--;
        int t = new_tmp(g);
        sb_printf(g->out, "%s t%d = L%c%d.a[k%d];\n", LIST_NUM(lt) ? "double" : "int", t, LCHAR(lt),
                  n->as.index.object->as.var.index, k);
        snprintf(res, 64, "t%d", t);
        return LIST_NUM(lt) ? NT_NUM : NT_BOOL;
    }
    default:  /* text, objects, closures...: not native */
        return fail(g);
    }
}

/* ---------- statements ---------- */

/* xs[i] op= value, xs a list of the fn's own: the index first, then the value, as the VM does. */
static void gen_item_assign(Gen *g, Node *n) {
    Node *target = n->as.assign.target, *list = target->as.index.object;
    int lt = list_slot(g, list), k = lt ? gen_index(g, list->as.var.index, lt, target->as.index.index) : 0;
    char v[64], d[64], cur[64], r[64], L = LCHAR(lt);
    int t = k ? gen_expr(g, n->as.assign.value, v) : 0, s = list->as.var.index;
    unbox(g, &t, v, LIST_NUM(lt) ? NT_NUM : NT_BOOL);
    TokenType op = n->as.assign.op;
    if (!t || (LIST_NUM(lt) ? !NUMERIC(t) : t != NT_BOOL || op != T_ASSIGN)) {
        fail(g);
        return;
    }
    if (op != T_ASSIGN) {  /* (Numbers) */
        TokenType bin = op == T_PLUS_EQ ? T_PLUS : op == T_MINUS_EQ ? T_MINUS : op == T_STAR_EQ ? T_STAR
                      : op == T_SLASH_EQ ? T_SLASH : T_PERCENT;
        snprintf(cur, sizeof cur, "Ln%d.a[k%d]", s, k - 1);
        t = gen_binop(g, bin, NT_NUM, cur, t, v, r);
        snprintf(v, sizeof v, "%s", r);
    }
    if (LIST_NUM(lt)) as_double(t, v, d);
    else snprintf(d, sizeof d, "%s", v);
    sb_printf(g->out, "L%c%d.a[k%d] = %s;\n", L, s, k - 1, d);
}

/* xs[i].x op= value, r.x op= value: a record's field (a Number). */
static void gen_field_assign(Gen *g, Node *n) {
    char place[64], v[64], r[64];
    int t = field_place(g, n->as.assign.target, place) ? gen_expr(g, n->as.assign.value, v) : 0;
    unbox(g, &t, v, NT_NUM);
    TokenType op = n->as.assign.op;
    if (!NUMERIC(t)) {
        fail(g);
        return;
    }
    if (op != T_ASSIGN) {
        TokenType bin = op == T_PLUS_EQ ? T_PLUS : op == T_MINUS_EQ ? T_MINUS : op == T_STAR_EQ ? T_STAR
                      : op == T_SLASH_EQ ? T_SLASH : T_PERCENT;
        t = gen_binop(g, bin, NT_NUM, place, t, v, r);
        snprintf(v, sizeof v, "%s", r);
    }
    as_double(t, v, r);
    sb_printf(g->out, "%s = %s;\n", place, r);
}

static void gen_assign(Gen *g, Node *n) {
    Node *target = n->as.assign.target;
    char v[64], r[64], cur[64];
    int t;
    if (target->kind == N_FIELD) {
        gen_field_assign(g, n);
        return;
    }
    if (target->kind == N_INDEX) {
        gen_item_assign(g, n);
        return;
    }
    if (target->kind == N_VAR && target->as.var.kind == VAR_LOCAL && n->as.assign.op == T_ASSIGN &&
        is_list_expr(n->as.assign.value)) {  /* xs = [..]: a list of the fn's own again */
        gen_list_into(g, target->as.var.index, n->as.assign.value);
        return;
    }
    if (target->kind != N_VAR || target->as.var.kind != VAR_LOCAL || !(t = gen_expr(g, n->as.assign.value, v))) {
        fail(g);
        return;
    }
    int slot = target->as.var.index, st = type_of(g, slot);
    TokenType op = n->as.assign.op;
    if (t == NT_VAL && st == NT_VAL && op == T_ASSIGN && strncmp(v, "vt", 2)) {  /* a value variable: another borrowed one */
        sb_printf(g->out, "%c%d = %s;\n", slot < g->cur->d->nparams ? 'p' : 'V', slot, v);
        return;
    }
    unbox(g, &t, v, st == NT_BOOL ? NT_BOOL : NT_NUM);
    if (st == UNTYPED) st = loop_scalar(g, slot, op == T_ASSIGN ? t : NT_NUM);  /* (a loop's global) */
    if (op == T_ASSIGN) {
        store(g, slot, t, v);
        return;
    }
    if (g->acc[slot] && st == NT_INT && t == NT_INT && (op == T_PLUS_EQ || op == T_MINUS_EQ)) {  /* (see loop_accs) */
        if (op == T_MINUS_EQ || !nonneg(g, v)) mark_neg(g, slot);
        sb_printf(g->out, "i%d = (int64_t)((uint64_t)i%d %c (uint64_t)%s);\novf%d |= LUBE_OUT(i%d);\n", slot, slot,
                  op == T_PLUS_EQ ? '+' : '-', v, g->acc[slot] - 1, slot);
        return;
    }
    TokenType bin = op == T_PLUS_EQ ? T_PLUS : op == T_MINUS_EQ ? T_MINUS : op == T_STAR_EQ ? T_STAR
                  : op == T_SLASH_EQ ? T_SLASH : T_PERCENT;
    snprintf(cur, sizeof cur, "%c%d", letter(st), slot);
    int rt = st && NUMERIC(st) && NUMERIC(t) ? gen_binop(g, bin, st, cur, t, v, r) : fail(g);
    if (rt) store(g, slot, rt, r);
}

static int writes_list(NodeList *l, int s);

/* Might code n change something in slot s (s = .., s.x = .., s[i].x = ..,
 * f(&s), f(&s[i]), s.method())? (Code that cannot be native says yes.) */
static int writes_to(Node *n, int s) {
    Node *v;
    if (!n) return 0;
    switch (n->kind) {
    case N_LITERAL: case N_VAR: case N_BREAK: case N_CONTINUE:
        return 0;
    case N_ASSIGN:
        v = place_var(n->as.assign.target);
        return (v && v->as.var.index == s) || writes_to(n->as.assign.target, s) || writes_to(n->as.assign.value, s);
    case N_REF:
        v = place_var(n->as.expr);
        return (v && v->as.var.index == s) || writes_to(n->as.expr, s);
    case N_METHOD:  /* (a method may change its object) */
        v = place_var(n->as.method.object);
        return (v && v->as.var.index == s) || writes_to(n->as.method.object, s) || writes_list(&n->as.method.args, s);
    case N_UNARY: return writes_to(n->as.unary.operand, s);
    case N_BINARY: case N_AND: case N_OR: return writes_to(n->as.binary.left, s) || writes_to(n->as.binary.right, s);
    case N_CALL: return writes_to(n->as.call.callee, s) || writes_list(&n->as.call.args, s);
    case N_INDEX: return writes_to(n->as.index.object, s) || writes_to(n->as.index.index, s);
    case N_FIELD: return writes_to(n->as.field.object, s);
    case N_RANGE: return writes_to(n->as.range.start, s) || writes_to(n->as.range.end, s);
    case N_LIST: return writes_list(&n->as.items, s);
    case N_TYPED_LIST: return writes_list(&n->as.tlist.items, s);
    case N_EXPR: case N_RETURN: return writes_to(n->as.expr, s);
    case N_LET: return !n->as.let.is_fn && writes_to(n->as.let.value, s);
    case N_BLOCK: return writes_list(&n->as.block.stmts, s);
    case N_IF:
        return writes_to(n->as.if_s.cond, s) || writes_to(n->as.if_s.then_branch, s) || writes_to(n->as.if_s.else_branch, s);
    case N_WHILE: return writes_to(n->as.while_s.cond, s) || writes_to(n->as.while_s.body, s);
    case N_FOR: return writes_to(n->as.for_s.iter, s) || writes_to(n->as.for_s.body, s);
    default: return 1;
    }
}

static int writes_list(NodeList *l, int s) {
    for (int i = 0; i < l->count; i++)
        if (writes_to(l->items[i], s)) return 1;
    return 0;
}

static int acc_list(NodeList *l, int s);

/* How code n uses slot s: only as an accumulator (s += x, s -= x, x not
 * reading s): how many times; -1 if any other way (or code it does not know). */
static int acc_uses(Node *n, int s) {
    int a, b;
    if (!n) return 0;
    switch (n->kind) {
    case N_LITERAL: case N_BREAK: case N_CONTINUE: return 0;
    case N_VAR: return n->as.var.kind == VAR_LOCAL && n->as.var.index == s ? -1 : 0;
    case N_ASSIGN: {
        Node *t = n->as.assign.target;
        if (t->kind == N_VAR && t->as.var.kind == VAR_LOCAL && t->as.var.index == s)
            return (n->as.assign.op == T_PLUS_EQ || n->as.assign.op == T_MINUS_EQ) && !acc_uses(n->as.assign.value, s) ? 1 : -1;
        a = acc_uses(t, s);
        b = acc_uses(n->as.assign.value, s);
        break;
    }
    case N_UNARY: return acc_uses(n->as.unary.operand, s);
    case N_BINARY: case N_AND: case N_OR:
        a = acc_uses(n->as.binary.left, s);
        b = acc_uses(n->as.binary.right, s);
        break;
    case N_CALL:
        a = acc_uses(n->as.call.callee, s);
        b = acc_list(&n->as.call.args, s);
        break;
    case N_METHOD:
        a = acc_uses(n->as.method.object, s);
        b = acc_list(&n->as.method.args, s);
        break;
    case N_INDEX:
        a = acc_uses(n->as.index.object, s);
        b = acc_uses(n->as.index.index, s);
        break;
    case N_FIELD: return acc_uses(n->as.field.object, s);
    case N_RANGE:
        a = acc_uses(n->as.range.start, s);
        b = acc_uses(n->as.range.end, s);
        break;
    case N_REF: case N_EXPR: case N_RETURN: return acc_uses(n->as.expr, s);
    case N_LIST: return acc_list(&n->as.items, s);
    case N_TYPED_LIST: return acc_list(&n->as.tlist.items, s);
    case N_LET: return n->as.let.is_fn ? -1 : acc_uses(n->as.let.value, s);
    case N_BLOCK: return acc_list(&n->as.block.stmts, s);
    case N_IF:
        a = acc_uses(n->as.if_s.cond, s);
        b = acc_uses(n->as.if_s.then_branch, s);
        if (a < 0 || b < 0) return -1;
        a += b;
        b = acc_uses(n->as.if_s.else_branch, s);
        break;
    case N_WHILE:
        a = acc_uses(n->as.while_s.cond, s);
        b = acc_uses(n->as.while_s.body, s);
        break;
    case N_FOR:
        a = acc_uses(n->as.for_s.iter, s);
        b = acc_uses(n->as.for_s.body, s);
        break;
    default: return -1;
    }
    return a < 0 || b < 0 ? -1 : a + b;
}

static int acc_list(NodeList *l, int s) {
    int c = 0;
    for (int i = 0; i < l->count; i++) {
        int k = acc_uses(l->items[i], s);
        if (k < 0) return -1;
        c += k;
    }
    return c;
}

/* Loop n's accumulators: whole-number variables it only adds to (total +=
 * x). Nothing in it depends on them, so their bound (2^53) is checked once,
 * after it (they add with wraparound meanwhile, sticking in ovf<k> when out
 * of bounds): a return in it cannot be using them. Returns k (-1: none). */
static int loop_accs(Gen *g, Node *n) {
    int k = -1, nslots = g->cur->d->nslots;
    if (g->fast) return -1;  /* (nothing checked: plain additions) */
    for (int s = 0; s < nslots; s++) {
        if (type_of(g, s) != NT_INT || g->acc[s] || acc_uses(n, s) <= 0) continue;
        if (k < 0) {
            k = new_tmp(g);
            sb_printf(g->out, "uint64_t ovf%d = 0;\n", k);
        }
        g->acc[s] = k + 1;
    }
    return k;
}

static void loop_accs_end(Gen *g, int k) {
    if (k < 0) return;
    for (int s = 0; s < g->cur->d->nslots; s++)
        if (g->acc[s] == k + 1) g->acc[s] = 0;
    sb_printf(g->out, "if (ovf%d) %s\n", k, bail(g));
}

static void gen_block(Gen *g, NodeList *stmts) {
    for (int i = 0; i < stmts->count && !g->failed; i++) gen_stmt(g, stmts->items[i]);
}

static void gen_stmt(Gen *g, Node *n) {
    char v[64], e[64], x[64], y[64];
    if (!g->failed) g->line = n->line;
    switch (n->kind) {
    case N_EXPR: {
        Node *x = n->as.expr;
        if (x->kind == N_ASSIGN) gen_assign(g, x);
        else if (x->kind == N_CALL) gen_call(g, x, v, 0);
        else if (x->kind == N_METHOD && !strcmp(x->as.method.name->chars, "push") && x->as.method.args.count == 1 &&
                 x->as.method.fallback.kind == VAR_GLOBAL && !map_get(g->declared, OBJ_VAL(x->as.method.fallback.name), NULL))
            gen_push(g, x->as.method.object, x->as.method.args.items[0]);  /* xs.push(v): the built-in push */
        else if (x->kind == N_METHOD) gen_method(g, x, v, 0);
        else gen_expr(g, x, v);
    } break;
    case N_LET: {
        Node *val = n->as.let.value;
        int rs = !n->as.let.is_fn && val->kind == N_INDEX ? record(g, val->as.index.object, NT_RLIST) : 0;
        if (rs) {  /* let r = xs[i]: a copy of the record */
            char p[64];
            int s = n->as.let.var.index, sh = g->dshape[g->live[rs - 1]];
            if (!row_place(g, val, p)) break;
            declare(g, &n->as.let.var, s);
            record_copy(g, s, sh, p);
            break;
        }
        declare(g, &n->as.let.var, n->as.let.var.index);
        if (!n->as.let.is_fn && is_list_expr(n->as.let.value)) {  /* a list of the fn's own */
            gen_list_into(g, n->as.let.var.index, n->as.let.value);
            break;
        }
        int t = n->as.let.is_fn ? 0 : gen_expr(g, n->as.let.value, v), s = n->as.let.var.index;
        int *dt = &g->dtypes[g->live[s]];
        if (t == NT_VAL && strncmp(v, "vt", 2) && (!*dt || *dt == NT_VAL)) {  /* let x = v[i]: a borrowed value */
            *dt = NT_VAL;
            sb_printf(g->out, "V%d = %s;\n", s, v);
        } else if (t) {
            store(g, s, t, v);
        } else {
            fail(g);
        }
    } break;
    case N_BLOCK:
        sb_puts(g->out, "{\n");
        gen_block(g, &n->as.block.stmts);
        sb_puts(g->out, "}\n");
        break;
    case N_IF:
        if (bool_expr(g, n->as.if_s.cond, v) != NT_BOOL) {
            fail(g);
            break;
        }
        sb_printf(g->out, "if (%s) ", v);
        gen_stmt(g, n->as.if_s.then_branch);
        if (n->as.if_s.else_branch) {
            sb_puts(g->out, "else {\n");
            gen_stmt(g, n->as.if_s.else_branch);
            sb_puts(g->out, "}\n");
        }
        break;
    case N_WHILE: {  /* the condition again on each turn (and after continue) */
        int acc = loop_accs(g, n);
        sb_puts(g->out, "for (;;) {\n");
        if (bool_expr(g, n->as.while_s.cond, v) != NT_BOOL) {
            fail(g);
            break;
        }
        sb_printf(g->out, "if (!%s) break;\n", v);
        gen_stmt(g, n->as.while_s.body);
        sb_puts(g->out, "}\n");
        loop_accs_end(g, acc);
        break;
    }
    case N_FOR: {  /* over a range: the same items as the VM's (see range_length); whole from a whole start */
        Node *it = n->as.for_s.iter;
        int rs = it->kind == N_VAR ? record(g, it, NT_RLIST) : 0;
        if (rs) {  /* for r in xs, xs a record list: each a copy (of xs as it was: it may not change in the loop) */
            int s = rs - 1, sh = g->dshape[g->live[s]], k = new_tmp(g), first = n->as.for_s.first.index;
            if (writes_to(n->as.for_s.body, s)) {
                fail(g);
                break;
            }
            sb_printf(g->out, "for (int64_t k%d = 0; k%d < Rn%d; k%d++) {\n", k, k, s, k);
            declare(g, &n->as.for_s.first, first);
            if (n->as.for_s.two) {  /* for i, r in xs */
                snprintf(y, sizeof y, "k%d", k);
                store(g, first, NT_INT, y);
                first = n->as.for_s.second.index;
                declare(g, &n->as.for_s.second, first);
            }
            snprintf(x, sizeof x, "R%d + k%d * %d", s, k, SHAPE(g, sh)->n);
            record_copy(g, first, sh, x);
            gen_block(g, &n->as.for_s.body->as.block.stmts);
            sb_puts(g->out, "}\n");
            break;
        }
        int ts = it->kind == N_RANGE ? gen_expr(g, it->as.range.start, v) : 0;
        int te = NUMERIC(ts) ? gen_expr(g, it->as.range.end, e) : 0;
        if (!NUMERIC(te)) {
            fail(g);
            break;
        }
        int acc = loop_accs(g, n), k = new_tmp(g);
        as_double(ts, v, x);
        as_double(te, e, y);
        sb_printf(g->out, "{\n%s s%d = %s;\ndouble n%d = lube_nrange(%s, %s, %d);\n", ctype(ts), k, v, k, x, y,
                  it->as.range.inclusive);
        if (ts == NT_INT) sb_printf(g->out, "if ((double)s%d + n%d > LUBE_BIGD) %s\n", k, k, bail(g));
        sb_printf(g->out, "for (int64_t k%d = 0; (double)k%d < n%d; k%d++) {\n", k, k, k, k);
        snprintf(x, sizeof x, ts == NT_INT ? "s%d + k%d" : "s%d + (double)k%d", k, k);
        if (ts == NT_INT) {  /* (as a temporary: not negative when the start is not) */
            int u = new_tmp(g);
            sb_printf(g->out, "int64_t t%d = %s;\n", u, x);
            g->tnn[u] = nonneg(g, v);
            snprintf(x, sizeof x, "t%d", u);
        }
        declare(g, &n->as.for_s.first, n->as.for_s.first.index);
        if (n->as.for_s.two) declare(g, &n->as.for_s.second, n->as.for_s.second.index);
        if (n->as.for_s.two) {  /* for i, x in a..b: the index, then the item */
            snprintf(y, sizeof y, "k%d", k);
            store(g, n->as.for_s.first.index, NT_INT, y);
            store(g, n->as.for_s.second.index, ts, x);
        } else {
            store(g, n->as.for_s.first.index, ts, x);
        }
        gen_block(g, &n->as.for_s.body->as.block.stmts);
        sb_puts(g->out, "}\n}\n");
        loop_accs_end(g, acc);
        break;
    }
    case N_RETURN: {
        Cand *c = g->cur;
        if (c->ret == NT_NONE && !c->d->ret.set && n->as.expr && !c->loop && !c->method) {
            c->ret = NT_VAL;  /* (no result type written, a value returned: a value it returns) */
            g->widened = 1;
        }
        int want = c->ret, t = n->as.expr ? gen_expr(g, n->as.expr, v) : NT_NONE;
        if (n->as.expr && !t) break;
        if (want == NT_VAL) {  /* (return alone: nothing, which the VM gives) */
            char bx[64];
            if (!n->as.expr) sb_printf(g->out, "%s\n", bail(g));
            else if (!boxed(t, v, bx)) fail(g);
            else if (t == NT_VAL && strncmp(v, "vt", 2)) sb_printf(g->out, "{\n*out = retain(%s);\nR = 0;\ngoto done;\n}\n", bx);
            else if (t == NT_VAL) sb_printf(g->out, "{\n*out = %s;\n%s = NIL_VAL;\nR = 0;\ngoto done;\n}\n", v, v);  /* (given away) */
            else sb_printf(g->out, "{\n*out = %s;\nR = 0;\ngoto done;\n}\n", bx);
            break;
        }
        unbox(g, &t, v, want);
        if (want == NT_NUM ? !NUMERIC(t) || (g->mode && t != NT_INT) : t != want) {  /* (whole: an int) */
            fail(g);
            break;
        }
        if (want == NT_NONE) sb_puts(g->out, "{\nR = 0;\ngoto done;\n}\n");
        else if (g->mode) sb_printf(g->out, "{\nR = %s;\ngoto done;\n}\n", v);
        else if (as_double(t, v, x), 1) sb_printf(g->out, "{\n*out = %s;\nR = 0;\ngoto done;\n}\n", x);
        break;
    }
    case N_BREAK:
        sb_puts(g->out, "break;\n");
        break;
    case N_CONTINUE:
        sb_puts(g->out, "continue;\n");
        break;
    default:  /* try, throw, on, ...: not native */
        fail(g);
        break;
    }
}

/* One pass over candidate c's body, in g->mode. */
static void gen_pass(Gen *g, Cand *c, StrBuf *out) {
    FuncDecl *d = c->d;
    int nslots = d->nslots > 0 ? d->nslots : 1, k = (int)(c - g->cands);
    g->out = out;
    g->tmp = 0;
    g->nvt = 0;
    g->widened = 0;
    g->line = d->line;  /* (where it stops, if before any of its code) */
    if (g->mode && c->ret == NT_VAL) fail(g);  /* (a value: no whole-number version) */
    for (int i = 0; i < nslots; i++) g->live[i] = -1, g->acc[i] = 0;
    if (g->mode) sb_printf(out, "static int64_t ni_%d(", k);
    else sb_printf(out, "static int nf_%d(", k);
    for (int i = 0; i < d->nparams; i++) {
        int t = c->params[i];
        if (t == NT_REC) sb_printf(out, "double *r%d, ", i);
        else if (t == NT_VAL) sb_printf(out, "Value p%d, ", i);
        else if (t == UNTYPED || t == NT_RLIST) sb_printf(out, "double *r%d, int64_t rn%d, ", i, i);
        else if (t >= NT_LNUM) sb_printf(out, "%s *pl%d, ", LTYPE(t), i);  /* (a loop's list: given back at the end) */
        else if (c->loop) sb_printf(out, "double *p%d, ", i);  /* (a global: given back at the end) */
        else sb_printf(out, "%s p%d, ", g->mode ? "int64_t" : "double", i);
    }
    /* one way out (done:), where the lists are let go; R: the result, or that it gave up */
    sb_printf(out, "%sint left) {\n%s R = %s;\n", g->mode ? "" : c->ret == NT_VAL ? "Value *out, " : "double *out, ",
              g->mode ? "int64_t" : "int", g->mode ? "LUBE_FAIL" : "1");
    sb_puts(out, "LubeNNum Lnt = {0, 0, 0};\nLubeNBool Lbt = {0, 0, 0};\n");
    for (int i = 0; i < nslots; i++)
        sb_printf(out, "double d%d = 0; int64_t i%d = 0; int b%d = 0; LubeNNum Ln%d = {0, 0, 0}; LubeNBool Lb%d = {0, 0, 0};\n"
                       "double *R%d = 0; int64_t Rn%d = 0; double *Bp%d = 0; double Bs%d[LUBE_REC_MAX]; Value V%d = NIL_VAL;\n"
                       "(void)d%d; (void)i%d; (void)b%d; (void)R%d; (void)Rn%d; (void)Bp%d; (void)Bs%d; (void)V%d;\n",
                  i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
    StrBuf body;  /* (after it: the values this fn makes, which the body says) */
    sb_init(&body);
    g->out = &body;
    sb_puts(&body, "if (left <= 0) goto done;\n");
    for (int i = 0; i < d->nparams; i++) {
        char p[64];
        int t = c->params[i];
        declare(g, &d->params[i], i);
        if (t == NT_VAL) {  /* a value: the caller's, borrowed */
            g->dtypes[g->live[i]] = NT_VAL;
            continue;
        }
        if (t >= NT_LNUM && t <= NT_TLBOOL) {  /* a loop's list: its array is this fn's until it gives it back */
            g->dtypes[g->live[i]] = t;
            sb_printf(&body, "L%c%d = *pl%d;\n", LCHAR(t), i, i);
            continue;
        }
        if (t == UNTYPED || RECORD(t)) {  /* a record parameter: the wrapper's copy of its fields */
            g->dtypes[g->live[i]] = t;
            g->dshape[g->live[i]] = c->shape[i];
            if (t == NT_REC) sb_printf(&body, "Bp%d = r%d;\n", i, i);
            else sb_printf(&body, "R%d = r%d;\nRn%d = rn%d;\n", i, i, i, i);
            continue;
        }
        if (c->loop) snprintf(p, sizeof p, t == NT_BOOL ? "(*p%d != 0)" : "(*p%d)", i);
        else snprintf(p, sizeof p, t == NT_BOOL ? "(p%d != 0)" : "p%d", i);
        store(g, i, t == NT_NUM && g->mode ? NT_INT : t, p);
        mark_neg(g, i);  /* (a parameter: any) */
    }
    if (d->is_arrow) {  /* fn f(x: Number): Number => x * x */
        Node ret;
        memset(&ret, 0, sizeof ret);
        ret.kind = N_RETURN;
        ret.as.expr = d->body;
        if (c->ret == NT_NONE && (c->loop || c->method)) fail(g);
        else gen_stmt(g, &ret);  /* (no result type: the value it returns) */
    } else {
        gen_block(g, &d->body->as.block.stmts);
    }
    /* (fell off the end: nothing to return, or the VM's error that the value is missing) */
    if (c->ret == NT_NONE) sb_puts(&body, "R = 0;\n");
    g->out = out;
    for (int i = 0; i < g->nvt; i++) sb_printf(out, "Value vt%d = NIL_VAL;\n", i);
    sb_putn(out, body.data, body.len);
    sb_free(&body);
    sb_puts(out, "done:\nfree(Lnt.a);\nfree(Lbt.a);\n");
    for (int i = 0; i < g->nvt; i++) sb_printf(out, "release(vt%d);\n", i);
    for (int i = 0; c->loop && i < d->nparams; i++) {  /* a loop's Numbers, Bools and lists go back to its globals */
        int t = c->params[i];
        if (t >= NT_LNUM && t <= NT_TLBOOL)
            sb_printf(out, "if (R == 0) {\n*pl%d = L%c%d;\nL%c%d.a = 0;\n}\n", i, LCHAR(t), i, LCHAR(t), i);
        else if (!RECORD(t) && t != UNTYPED && c->written[i])
            sb_printf(out, "if (R == 0) *p%d = %c%d;\n", i, letter(type_of(g, i)), i);
    }
    for (int i = 0; i < nslots; i++) sb_printf(out, "free(Ln%d.a);\nfree(Lb%d.a);\n", i, i);
    sb_puts(out, "return R;\n}\n\n");
}

/* Translates candidate c (in mode) into out, translating again while the
 * slots' types widen; 0 if it cannot be native this way. */
static int gen_function(Gen *g, Cand *c, int mode, StrBuf *out) {
    int nslots = c->d->nslots > 0 ? c->d->nslots : 1;
    g->cur = c;
    g->mode = mode;
    g->failed = 0;
    g->live = xmalloc(sizeof(int) * (size_t)nslots);
    g->acc = xmalloc(sizeof(int) * (size_t)nslots);
    g->ndecls = 0;
    StrBuf pass;
    do {
        sb_init(&pass);
        gen_pass(g, c, &pass);
        if (!g->widened || g->failed) break;
        sb_free(&pass);
    } while (1);
    for (int i = 0; i < c->d->nparams; i++)
        if (c->params[i] == UNTYPED) fail(g);  /* (an untyped parameter that is not a record: not native) */
    if (!g->failed) sb_putn(out, pass.data, pass.len);
    sb_free(&pass);
    free(g->live);
    free(g->acc);
    return !g->failed;
}

/* ---------- the program ---------- */

/* Has candidate k a parameter without a written type? */
static int untyped_params(Cand *k) {
    for (int p = 0; p < k->d->nparams; p++)
        if (!(k->d->types && k->d->types[p].set)) return 1;
    return 0;
}

/* A fn the program declares that can be a candidate: Number and Bool in and out. */
static void consider(Gen *g, Node *s, uint64_t hash) {
    if (s->kind != N_LET || !s->as.let.is_fn) return;
    FuncDecl *d = s->as.let.value->as.func;
    if (d->nparams > NATIVE_MAX_PARAMS) return;
    Cand c;
    int records = 0;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < d->nparams; i++) {
        int typed = d->types && d->types[i].set;
        c.params[i] = typed ? ann_type(&d->types[i]) : UNTYPED;  /* (untyped: perhaps a record) */
        if (d->defaults[i] || (typed && (c.params[i] <= NT_NONE || d->modes[i] == PARAM_REF))) return;
        if (!typed) {
            g->shapes = xrealloc(g->shapes, sizeof(*g->shapes) * (size_t)(g->nshapes + 1));
            g->shapes[g->nshapes].n = 0;
            g->shapes[g->nshapes].parent = g->nshapes;
            c.shape[i] = g->nshapes++;
            records = 1;
        }
    }
    c.ret = ann_type(&d->ret);
    if (c.ret < 0) return;
    c.d = d;
    c.key = s->as.let.var.name;
    c.hash = hash;
    c.ok = 1;
    c.ok_int = !records;  /* (a whole-number version only for Numbers and Bools) */
    g->cands = xrealloc(g->cands, sizeof(Cand) * (size_t)(g->ncands + 1));
    g->cands[g->ncands++] = c;
}

/* A method of class k (an N_FUNC), Numbers, Bools and records in and out:
 * a candidate, its `this` a record of exactly that class. */
static void consider_method(Gen *g, Interp *I, int k, Node *f, uint64_t hash) {
    FuncDecl *d = f->as.func;
    if (!d->is_method || d->is_ctor || d->nparams > NATIVE_MAX_PARAMS) return;
    Cand c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < d->nparams; i++) {
        int typed = i > 0 && d->types && d->types[i].set;
        c.params[i] = typed ? ann_type(&d->types[i]) : UNTYPED;
        if (d->defaults[i] || (typed && (c.params[i] <= NT_NONE || d->modes[i] == PARAM_REF))) return;
        if (!typed) {
            g->shapes = xrealloc(g->shapes, sizeof(*g->shapes) * (size_t)(g->nshapes + 1));
            g->shapes[g->nshapes].n = 0;
            g->shapes[g->nshapes].parent = g->nshapes;
            c.shape[i] = g->nshapes++;
        }
    }
    c.params[0] = NT_REC;
    c.cls[0] = k + 1;
    c.ret = ann_type(&d->ret);
    if (c.ret < 0) return;
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "%s.%s", g->classes[k].key->chars, d->name->chars);
    c.d = d;
    c.key = lube_intern(I, key.data, key.len);
    sb_free(&key);
    c.hash = hash;
    c.ok = 1;
    c.method = 1;
    g->cands = xrealloc(g->cands, sizeof(Cand) * (size_t)(g->ncands + 1));
    g->cands[g->ncands++] = c;
}

static void note_declared(Gen *g, Node *s) {
    ObjString *key = s->kind == N_LET ? s->as.let.var.name : s->kind == N_CLASS ? s->as.klass.var.name
                   : s->kind == N_IMPORT ? s->as.import.bind : NULL;
    if (!key) return;
    Value v;
    double n = map_get(g->declared, OBJ_VAL(key), &v) ? v.as.number : 0;
    map_set(g->declared, retain(OBJ_VAL(key)), NUM_VAL(n + 1));
    if (s->kind == N_LET && !s->as.let.is_fn) {  /* a variable: a Bool when it starts as one, or a list */
        Node *x = s->as.let.value, *l = x;
        if (l->kind == N_BINARY && l->as.binary.op == T_STAR)  /* ([x] * n) */
            l = l->as.binary.left->kind == N_LIST || l->as.binary.left->kind == N_TYPED_LIST ? l->as.binary.left : l->as.binary.right;
        Node *first = l->kind == N_LIST && l->as.items.count ? l->as.items.items[0] : NULL;
        int list = l->kind == N_TYPED_LIST ? (l->as.tlist.elem == TT_NUMBER ? NT_TLNUM : l->as.tlist.elem == TT_BOOL ? NT_TLBOOL : 0)
                 : first && first->kind == N_LITERAL ? (IS_NUM(first->as.literal) ? NT_LNUM : IS_BOOL(first->as.literal) ? NT_LBOOL : 0)
                 : 0;
        int b = x->kind == N_LITERAL ? IS_BOOL(x->as.literal)
              : x->kind == N_AND || x->kind == N_OR || (x->kind == N_UNARY && x->as.unary.op == T_NOT)
              || (x->kind == N_BINARY && (x->as.binary.op == T_LT || x->as.binary.op == T_LE || x->as.binary.op == T_GT
                                          || x->as.binary.op == T_GE || x->as.binary.op == T_EQ || x->as.binary.op == T_NEQ));
        map_set(g->vars, retain(OBJ_VAL(key)), NUM_VAL((list ? list : b ? NT_BOOL : NT_NUM) + (s->as.let.is_var ? 0 : 16)));
    }
    if (s->kind == N_CLASS) {
        g->classes = xrealloc(g->classes, sizeof(ClassInfo) * (size_t)(g->nclasses + 1));
        g->classes[g->nclasses].key = key;
        g->classes[g->nclasses++].node = s;
    }
}

/* ---------- top-level loops ---------- */

/* A top-level loop becomes a fn of the globals it uses: walked once to find
 * them (shift < 0), then again to make them its first slots (its
 * parameters) and move its own locals up past them. ok: only code that can
 * be native is in it. */
typedef struct {
    Gen *g;
    ObjString *names[NATIVE_MAX_PARAMS];
    int hint[NATIVE_MAX_PARAMS];
    int n, shift, ok;
} LoopVars;

static void loop_ref(LoopVars *lv, VarRef *v) {
    Value hint;
    if (v->kind == VAR_LOCAL) {
        if (lv->shift >= 0) v->index += lv->shift;
        return;
    }
    Value n;
    if (v->kind != VAR_GLOBAL || !map_get(lv->g->vars, OBJ_VAL(v->name), &hint) ||
        !map_get(lv->g->declared, OBJ_VAL(v->name), &n) || n.as.number > 1)
        return;  /* a fn, a class, a built-in...: left as it is (translating it fails) */
    int i = 0;
    while (i < lv->n && lv->names[i] != v->name) i++;
    if (lv->shift < 0) {
        if (i < lv->n) return;
        if (lv->n == NATIVE_MAX_PARAMS) {
            lv->ok = 0;
            return;
        }
        lv->hint[lv->n] = (int)hint.as.number;  /* (+ 16: let) */
        lv->names[lv->n++] = v->name;
        return;
    }
    v->kind = VAR_LOCAL;
    v->index = i;
}

static void loop_walk(LoopVars *lv, Node *n);

static void loop_walk_list(LoopVars *lv, NodeList *l) {
    for (int i = 0; i < l->count; i++) loop_walk(lv, l->items[i]);
}

static void loop_walk(LoopVars *lv, Node *n) {
    if (!n || !lv->ok) return;
    switch (n->kind) {
    case N_LITERAL: case N_BREAK: case N_CONTINUE: break;
    case N_VAR: loop_ref(lv, &n->as.var); break;
    case N_ASSIGN:
        loop_walk(lv, n->as.assign.target);
        loop_walk(lv, n->as.assign.value);
        break;
    case N_UNARY: loop_walk(lv, n->as.unary.operand); break;
    case N_BINARY: case N_AND: case N_OR:
        loop_walk(lv, n->as.binary.left);
        loop_walk(lv, n->as.binary.right);
        break;
    case N_CALL:  /* (a fn called by name stays a global) */
        if (n->as.call.callee->kind != N_VAR || n->as.call.callee->as.var.kind != VAR_GLOBAL) lv->ok = 0;
        loop_walk_list(lv, &n->as.call.args);
        break;
    case N_METHOD:
        loop_walk(lv, n->as.method.object);
        loop_walk_list(lv, &n->as.method.args);
        break;
    case N_INDEX:
        loop_walk(lv, n->as.index.object);
        loop_walk(lv, n->as.index.index);
        break;
    case N_FIELD: loop_walk(lv, n->as.field.object); break;
    case N_RANGE:
        loop_walk(lv, n->as.range.start);
        loop_walk(lv, n->as.range.end);
        break;
    case N_REF: case N_EXPR: loop_walk(lv, n->as.expr); break;
    case N_LIST: loop_walk_list(lv, &n->as.items); break;
    case N_TYPED_LIST: loop_walk_list(lv, &n->as.tlist.items); break;
    case N_LET:
        if (n->as.let.is_fn || n->as.let.var.kind != VAR_LOCAL) lv->ok = 0;
        loop_ref(lv, &n->as.let.var);
        loop_walk(lv, n->as.let.value);
        break;
    case N_BLOCK: loop_walk_list(lv, &n->as.block.stmts); break;
    case N_IF:
        loop_walk(lv, n->as.if_s.cond);
        loop_walk(lv, n->as.if_s.then_branch);
        loop_walk(lv, n->as.if_s.else_branch);
        break;
    case N_WHILE:
        loop_walk(lv, n->as.while_s.cond);
        loop_walk(lv, n->as.while_s.body);
        break;
    case N_FOR:
        loop_ref(lv, &n->as.for_s.first);
        if (n->as.for_s.two) loop_ref(lv, &n->as.for_s.second);
        loop_walk(lv, n->as.for_s.iter);
        loop_walk(lv, n->as.for_s.body);
        break;
    default:  /* closures, try, text, maps...: not native */
        lv->ok = 0;
        break;
    }
}

/* Top-level statement `index` of a file (of `frame_slots` slots), when it is
 * a loop: a candidate, as a fn of the globals it uses. */
static void consider_loop(Gen *g, Interp *I, Node *s, int index, uint64_t hash, int frame_slots) {
    if (s->kind != N_FOR && s->kind != N_WHILE) return;
    LoopVars lv;
    memset(&lv, 0, sizeof lv);
    lv.g = g;
    lv.ok = 1;
    lv.shift = -1;
    loop_walk(&lv, s);
    if (!lv.ok) return;
    lv.shift = lv.n;
    loop_walk(&lv, s);
    FuncDecl *d = xmalloc(sizeof(FuncDecl));
    Node *body = xmalloc(sizeof(Node));
    memset(d, 0, sizeof *d);
    memset(body, 0, sizeof *body);
    body->kind = N_BLOCK;
    body->line = s->line;
    body->as.block.stmts.items = xmalloc(sizeof(Node *));
    body->as.block.stmts.items[0] = s;
    body->as.block.stmts.count = body->as.block.stmts.cap = 1;
    d->nparams = lv.n;
    d->params = xmalloc(sizeof(ObjString *) * (size_t)(lv.n + 1));
    d->modes = xmalloc((size_t)lv.n + 1);
    d->defaults = xmalloc(sizeof(Node *) * (size_t)(lv.n + 1));
    d->body = body;
    d->nslots = frame_slots + lv.n;
    d->line = s->line;
    Cand c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < lv.n; i++) {
        d->params[i] = lv.names[i];
        d->modes[i] = PARAM_REF;
        d->defaults[i] = NULL;
        c.params[i] = UNTYPED;  /* (its uses say: a Number, a Bool or a record) */
        c.hint[i] = lv.hint[i] & 15;
        c.fixed[i] = lv.hint[i] >= 16;
        c.written[i] = writes_to(s, i);
        g->shapes = xrealloc(g->shapes, sizeof(*g->shapes) * (size_t)(g->nshapes + 1));
        g->shapes[g->nshapes].n = 0;
        g->shapes[g->nshapes].parent = g->nshapes;
        c.shape[i] = g->nshapes++;
    }
    char key[32];
    snprintf(key, sizeof key, "@%d", index);
    c.d = d;
    c.key = lube_intern(I, key, strlen(key));
    c.hash = hash;
    c.ret = NT_NONE;
    c.ok = 1;
    c.loop = 1;
    g->cands = xrealloc(g->cands, sizeof(Cand) * (size_t)(g->ncands + 1));
    g->cands[g->ncands++] = c;
}

/* For a fn with record parameters: the wrapper the VM calls with the
 * arguments as they are. It copies each record's fields into an array
 * (gives up, to the VM, when an object lacks one or it is not a Number),
 * calls the machine code, and writes back to & parameters the fields that
 * changed. Returns 0 when the fn has no record parameter. */
static int records_wrapper(Gen *g, Cand *k, int i, StrBuf *c) {
    int recs = k->loop || k->ret == NT_VAL;  /* (a loop's wrapper takes its globals; a value goes back as it is) */
    for (int p = 0; p < k->d->nparams; p++) recs |= RECORD(k->params[p]) || k->params[p] == NT_VAL;
    if (!recs) return 0;
    if (k->loop) {
        sb_printf(c, "static const char *const gn_%d[] = {", i);
        for (int p = 0; p < k->d->nparams; p++) sb_printf(c, "\"%s\", ", k->d->params[p]->chars);
        sb_puts(c, "0};\n");
    }
    for (int p = 0; p < k->d->nparams; p++) {
        if (!RECORD(k->params[p])) continue;
        int sh = k->shape[p];
        sb_printf(c, "static const char *const fs_%d_%d[] = {", i, p);
        for (int f = 0; f < SHAPE(g, sh)->n; f++) sb_printf(c, "\"%s\", ", SHAPE(g, sh)->fields[f]->chars);
        sb_printf(c, "0};\nstatic LUBE_THREAD_LOCAL LubeRecCache rc_%d_%d;\n", i, p);
    }
    sb_printf(c, "static int wv_%d(Interp *I, Value *args, %s *out, int left) {\n    int R = 1, called = 0;\n    (void)called;\n", i,
              k->ret == NT_VAL ? "Value" : "double");
    if (k->loop && k->d->nparams) {  /* its globals, as the arguments (taken back at the end) */
        sb_printf(c, "    Value *gv[%d], ga[%d];\n    args = ga;\n", k->d->nparams, k->d->nparams);
        for (int p = 0; p < k->d->nparams; p++)
            sb_printf(c, "    if (!(gv[%d] = lube_native_global(I, gn_%d[%d], %d))) return 1;\n    ga[%d] = *gv[%d];\n", p, i,
                      p, k->written[p], p, p);
    }
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p];
        if (t == NT_RLIST)
            sb_printf(c, "    double rs%d[256], os%d[256], *r%d = rs%d, *o%d = os%d;\n    int64_t rn%d = 0;\n", p, p, p, p, p, p, p);
        else if (t == NT_REC) sb_printf(c, "    double rb%d[LUBE_REC_MAX], ob%d[LUBE_REC_MAX];\n", p, p);
        else if (t >= NT_LNUM && t <= NT_TLBOOL) sb_printf(c, "    %s l%d = {0, 0, 0};\n", LTYPE(t), p);
        else if (t == NT_VAL) continue;  /* (a value: the argument itself) */
        else sb_printf(c, "    double a%d = 0;\n", p);
    }
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p], nf = RECORD(t) ? SHAPE(g, k->shape[p])->n : 0;
        int ref = lent_param(k, p);  /* (& or this: keep what was loaded, to see what changed) */
        char cls[160];  /* (the class its records must be, when methods are called on them) */
        if (k->cls[p]) snprintf(cls, sizeof cls, "\"%s\"", g->classes[k->cls[p] - 1].key->chars);
        else strcpy(cls, "0");
        if (t == NT_RLIST) {
            sb_printf(c, "    if (lube_reclist_load(I, &rc_%d_%d, args[%d], fs_%d_%d, %d, %s, &r%d, &rn%d, rs%d, 256)) goto done;\n", i,
                      p, p, i, p, nf, cls, p, p, p);
            if (ref) sb_printf(c, "    if (r%d != rs%d) o%d = malloc(sizeof(double) * (size_t)(rn%d * %d + 1));\n"
                                  "    if (!o%d) goto done;\n    memcpy(o%d, r%d, sizeof(double) * (size_t)(rn%d * %d));\n",
                               p, p, p, p, nf, p, p, p, p, nf);
        } else if (t == NT_VAL) {
            continue;
        } else if (t >= NT_LNUM && t <= NT_TLBOOL) {  /* (a loop's list: its items, copied) */
            sb_printf(c, "    if (lube_numlist_load(args[%d], %d, %d, (void **)&l%d.a, &l%d.n)) goto done;\n    l%d.cap = l%d.n;\n",
                      p, !LIST_NUM(t), t == NT_TLNUM || t == NT_TLBOOL, p, p, p, p);
        } else if (t == NT_REC) {
            sb_printf(c, "    if (lube_rec_load(I, &rc_%d_%d, args[%d], fs_%d_%d, %d, %s, rb%d)) goto done;\n", i, p, p, i, p, nf, cls, p);
            if (ref) sb_printf(c, "    memcpy(ob%d, rb%d, sizeof(double) * %d);\n", p, p, nf);
        }
        else
            sb_printf(c, "    if (!IS_%s(args[%d])) goto done;\n    a%d = args[%d].as.%s;\n", t == NT_NUM ? "NUM" : "BOOL", p, p, p,
                      t == NT_NUM ? "number" : "boolean");
    }
    sb_printf(c, "    called = 1;\n    R = nf_%d(", i);  /* (its lists are the machine code's from here) */
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p];
        if (t == NT_RLIST) sb_printf(c, "r%d, rn%d, ", p, p);
        else if (t == NT_REC) sb_printf(c, "rb%d, ", p);
        else if (t >= NT_LNUM && t <= NT_TLBOOL) sb_printf(c, "&l%d, ", p);
        else if (t == NT_VAL) sb_printf(c, "args[%d], ", p);
        else sb_printf(c, k->loop ? "&a%d, " : "a%d, ", p);
    }
    sb_puts(c, "out, left);\n    if (R == 0) {\n");
    for (int p = 0; k->loop && p < k->d->nparams; p++) {  /* (its Numbers, Bools and lists: as the machine code left them) */
        int t = k->params[p];
        if (t >= NT_LNUM && t <= NT_TLBOOL) {
            if (k->written[p]) sb_printf(c, "        lube_numlist_store(&ga[%d], %d, l%d.a, l%d.n);\n        *gv[%d] = ga[%d];\n",
                                         p, !LIST_NUM(t), p, p, p, p);
            else sb_printf(c, "        free(l%d.a);\n", p);
        } else if (k->written[p] && !RECORD(t)) {
            sb_printf(c, "        *gv[%d] = %s_VAL(a%d%s);\n", p, t == NT_BOOL ? "BOOL" : "NUM", p, t == NT_BOOL ? " != 0" : "");
        }
    }
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p], nf = RECORD(t) ? SHAPE(g, k->shape[p])->n : 0;
        if (!RECORD(t) || !lent_param(k, p)) continue;  /* (& parameters and this only: the others were copies) */
        if (t == NT_RLIST)
            sb_printf(c, "        lube_reclist_store(I, &rc_%d_%d, &args[%d], %d, r%d, o%d);\n", i, p, p, nf, p, p);
        else
            sb_printf(c, "        lube_rec_store(I, &rc_%d_%d, &args[%d], %d, rb%d, ob%d);\n", i, p, p, nf, p, p);
        if (k->loop) sb_printf(c, "        *gv[%d] = ga[%d];\n", p, p);  /* (a copy, if the global's was shared) */
    }
    sb_puts(c, "    }\ndone:\n");
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p];
        if (t == NT_RLIST) sb_printf(c, "    if (r%d != rs%d) free(r%d);\n    if (o%d != os%d) free(o%d);\n", p, p, p, p, p, p);
        if (t >= NT_LNUM && t <= NT_TLBOOL) sb_printf(c, "    if (!called) free(l%d.a);\n", p);
    }
    sb_puts(c, "    return R;\n}\n\n");
    return 1;
}

/* Where Lube's C sources are: LUBE_SRC, or src next to this lube (as it was
 * run: argv0). NULL if they are not there. */
static char *find_sources(const char *argv0) {
    StrBuf dir;
    sb_init(&dir);
    const char *env = getenv("LUBE_SRC");
    if (env && *env) {
        sb_puts(&dir, env);
    } else {
        const char *slash = NULL;
        for (const char *p = argv0; *p; p++)
            if (*p == '/' || (*p == '\\' && LUBE_PATH_SEP == ';')) slash = p;  /* (\ too on Windows) */
        if (slash) sb_putn(&dir, argv0, (size_t)(slash - argv0 + 1));
        sb_puts(&dir, "src");
    }
    StrBuf probe;
    sb_init(&probe);
    sb_printf(&probe, "%s/lube.h", dir.data ? dir.data : "");
    FILE *f = fopen(probe.data, "rb");
    sb_free(&probe);
    if (f) {
        fclose(f);
        return dir.data;
    }
    sb_free(&dir);
    return NULL;
}

static const char *const HEADER[] = {  /* (pieces: C99 promises string literals of 4095 bytes only) */
    "/* Generated by lube --build --native: the program's functions that compute only with Numbers\n"
    " * and Bools, in C. Each returns 0 with its result in *out, or 1 to let the VM run the call. */\n"
    "#include \"lube.h\"\n#include <math.h>\n#include <stdlib.h>\n#include <string.h>\n\n"
    "#define LUBE_FAIL INT64_MIN  /* a whole-number version gave up (no Number it computes is this) */\n"
    "#define LUBE_BIG 9007199254740992LL  /* 2^53: whole Numbers up to this are exact, in a double and here */\n"
    "#define LUBE_BIGD 9007199254740992.0\n"
    "#define LUBE_OUT(t) ((uint64_t)(t) + (uint64_t)LUBE_BIG > 2 * (uint64_t)LUBE_BIG)  /* past 2^53, either way */\n"
    "static int lube_nwhole(double x) {  /* a Number the whole-number versions can take (not -0) */\n"
    "    return x >= -LUBE_BIGD && x <= LUBE_BIGD && x == floor(x) && !(x == 0 && signbit(x));\n"
    "}\n"
    "static double lube_nmod(double x, double y) {  /* x % y as Lube computes it (y is not 0) */\n"
    "    if (fabs(x) < 9e15 && fabs(y) < 9e15 && x == floor(x) && y == floor(y)) {\n"
    "        int64_t q = (int64_t)x % (int64_t)y;\n"
    "        if (q == 0) return copysign(0.0, x);\n"
    "        if ((q < 0) != (y < 0)) q += (int64_t)y;\n"
    "        return (double)q;\n"
    "    }\n"
    "    double r = fmod(x, y);\n"
    "    if (r != 0 && ((r < 0) != (y < 0))) r += y;\n"
    "    return r;\n"
    "}\n"
    "static double lube_nrange(double start, double end, int inclusive) {  /* see range_length */\n"
    "    double span = end - start;\n"
    "    if (span < 0) return 0;\n"
    "    double n = inclusive ? floor(span + 1e-9) + 1 : ceil(span - 1e-9);\n"
    "    return n > 0 ? n : 0;\n"
    "}\n"
    "static double lube_nround(double x) { return lube_round(x); }\n"
    ,
    "static int lube_nidx(int64_t n, double d, int64_t *k) {  /* a list position, as list_index finds it */\n"
    "    if (d != floor(d)) return 1;\n"
    "    if (d < 0) d += (double)n;\n"
    "    if (!(d >= 0 && d < (double)n)) return 1;\n"
    "    *k = (int64_t)d;\n"
    "    return 0;\n"
    "}\n"
    ,
    "/* the fn's own lists: push and repeat as the VM does (1: the VM says what is wrong) */\n"
    "#define LUBE_NLIST(T, E)                                                                  \\\n"
    "    typedef struct { E *a; int64_t n, cap; } T;                                           \\\n"
    "    static int T##_push(T *l, E v) {                                                      \\\n"
    "        if ((size_t)l->n >= LUBE_MAX_ITEMS) return 1;                                     \\\n"
    "        if (l->n == l->cap) {                                                             \\\n"
    "            int64_t cap = l->cap ? l->cap * 2 : 8;                                        \\\n"
    "            E *a = realloc(l->a, sizeof(E) * (size_t)cap);                                \\\n"
    "            if (!a) return 1;                                                             \\\n"
    "            l->a = a;                                                                     \\\n"
    "            l->cap = cap;                                                                 \\\n"
    "        }                                                                                 \\\n"
    "        l->a[l->n++] = v;                                                                 \\\n"
    "        return 0;                                                                         \\\n"
    "    }                                                                                     \\\n"
    "    static int T##_repeat(T *out, const T *src, double times) {                           \\\n"
    "        if (!(times >= 0) || times != floor(times)) return 1;                             \\\n"
    "        if (src->n == 0) times = 0;                                                       \\\n"
    "        if (times > (double)(LUBE_MAX_ITEMS / (size_t)(src->n ? src->n : 1))) return 1;   \\\n"
    "        int64_t k = (int64_t)times, total = k * src->n;                                   \\\n"
    "        E *a = malloc(sizeof(E) * (size_t)(total ? total : 1));                           \\\n"
    "        if (!a) return 1;                                                                 \\\n"
    "        for (int64_t i = 0; i < k; i++) memcpy(a + i * src->n, src->a, sizeof(E) * (size_t)src->n); \\\n"
    "        free(out->a);                                                                     \\\n"
    "        out->a = a;                                                                       \\\n"
    "        out->n = out->cap = total;                                                        \\\n"
    "        return 0;                                                                         \\\n"
    "    }\n"
    "LUBE_NLIST(LubeNNum, double)\n"
    "LUBE_NLIST(LubeNBool, unsigned char)\n"
    "static double lube_nlog2(double x, double base) { return lube_m_log(x) / lube_m_log(base); }\n"
    "static int lube_nv_len(Value v, int64_t *n) {  /* values (NT_VAL): a list's length... */\n"
    "    if (!IS_LIST(v)) return 1;\n"
    "    *n = (int64_t)AS_LIST(v)->count;\n"
    "    return 0;\n"
    "}\n"
    "static int lube_nv_index(Value v, double d, Value *out) {  /* ...an item (borrowed), as the VM finds it... */\n"
    "    int64_t k;\n"
    "    if (!IS_LIST(v) || lube_nidx((int64_t)AS_LIST(v)->count, d, &k)) return 1;\n"
    "    *out = AS_LIST(v)->items[k];\n"
    "    return 0;\n"
    "}\n"
    "static int lube_nv_num(Value v, double *d) {  /* ...a Number, a Bool (else: the VM runs the call) */\n"
    "    if (!IS_NUM(v)) return 1;\n"
    "    *d = v.as.number;\n"
    "    return 0;\n"
    "}\n"
    "static int lube_nv_bool(Value v, int *b) {\n"
    "    if (!IS_BOOL(v)) return 1;\n"
    "    *b = v.as.boolean;\n"
    "    return 0;\n"
    "}\n\n",
    NULL};

/* Compiles the program (its files: keys and texts, read from dir) into the
 * executable exe_out: Lube itself, with the functions that can be native
 * compiled to machine code. Returns 0, or an exit status after a message. */
int lube_native_build(Interp *I, int nfiles, char **keys, char **texts, size_t *lens, const char *argv0,
                      const char *exe_out, int fast) {
    Gen g;
    memset(&g, 0, sizeof g);
    g.fast = fast;
    g.declared = new_map();
    g.vars = new_map();
    Arena **arenas = xmalloc(sizeof(Arena *) * (size_t)nfiles);
    Node **trees = xmalloc(sizeof(Node *) * (size_t)nfiles);
    for (int i = 0; i < nfiles; i++) {  /* (each was checked when it was packed) */
        Source *src = lube_add_source(I, keys[i], xstrndup(texts[i], lens[i]), lens[i]);
        trees[i] = parse_program(I, src, &arenas[i]);
        resolve_program(I, trees[i], src, arenas[i]);
        NodeList *stmts = &trees[i]->as.block.stmts;
        for (int j = 0; j < stmts->count; j++) note_declared(&g, stmts->items[j]);
    }
    for (int i = 0; i < nfiles; i++) {
        NodeList *stmts = &trees[i]->as.block.stmts;
        uint64_t hash = lube_hash_text(texts[i], lens[i]);
        for (int j = 0; j < stmts->count; j++) consider(&g, stmts->items[j], hash);
    }
    for (int i = 0; i < nfiles; i++) {  /* the methods of classes declared once */
        NodeList *stmts = &trees[i]->as.block.stmts;
        uint64_t hash = lube_hash_text(texts[i], lens[i]);
        for (int j = 0; j < stmts->count; j++) {
            Node *s = stmts->items[j];
            Value n;
            if (s->kind != N_CLASS || !map_get(g.declared, OBJ_VAL(s->as.klass.var.name), &n) || n.as.number > 1) continue;
            int k = 0;
            while (g.classes[k].node != s) k++;
            for (int m = 0; m < s->as.klass.methods.count; m++) consider_method(&g, I, k, s->as.klass.methods.items[m], hash);
        }
    }
    for (int i = 0; i < nfiles; i++) {  /* (after the fns: a loop calls them) */
        NodeList *stmts = &trees[i]->as.block.stmts;
        uint64_t hash = lube_hash_text(texts[i], lens[i]);
        for (int j = 0; j < stmts->count; j++) consider_loop(&g, I, stmts->items[j], j, hash, trees[i]->as.block.frame_slots);
    }
    for (int i = 0; i < g.ncands; i++) {  /* a global declared twice may not be this fn when it is called */
        Value n;
        map_get(g.declared, OBJ_VAL(g.cands[i].key), &n);
        if (n.as.number > 1) g.cands[i].ok = 0;
    }
    StrBuf body;
    int changed = 1;
    for (int round = 0; changed; round++) {  /* drop the versions that cannot be, until the rest only call each other */
        int shapes = g.shape_changes;
        changed = 0;
        for (int i = 0; i < g.ncands; i++) {
            Cand *k = &g.cands[i];
            for (int mode = 0; mode < 2; mode++) {
                if (!(mode ? k->ok_int : k->ok)) continue;
                sb_init(&body);
                if (!gen_function(&g, k, mode, &body) && round == 0) {
                    changed = 1;  /* (the first round learns what kinds of records fns pass each other) */
                } else if (g.failed && !mode && !k->vals && !k->loop && !k->method && untyped_params(k)) {
                    k->vals = 1;  /* (its untyped parameters cannot be records: values, then) */
                    for (int p = 0; p < k->d->nparams; p++)
                        if (!(k->d->types && k->d->types[p].set)) k->params[p] = NT_VAL, k->cls[p] = 0;
                    k->ok_int = 0;
                    changed = 1;
                } else if (g.failed) {
                    if (!mode) {
                        k->ok = 0;
                        k->why = g.line;
                    }
                    k->ok_int = 0;
                    changed = 1;
                }
                sb_free(&body);
            }
        }
        if (g.shape_changes != shapes) changed = 1;  /* (records grew: callers lay them out again) */
    }

    StrBuf c, names;
    sb_init(&c);
    sb_init(&names);
    for (int i = 0; HEADER[i]; i++) sb_puts(&c, HEADER[i]);
    int count = 0;
    for (int i = 0; i < g.ncands; i++) {
        Cand *k = &g.cands[i];
        if (!k->ok) continue;
        sb_printf(&c, "static int nf_%d(", i);
        for (int p = 0; p < k->d->nparams; p++)
            sb_puts(&c, k->params[p] == NT_REC ? "double *, " : k->params[p] == NT_RLIST ? "double *, int64_t, "
                        : k->params[p] == NT_VAL ? "Value, "
                        : k->params[p] == NT_LNUM || k->params[p] == NT_TLNUM ? "LubeNNum *, "
                        : k->params[p] == NT_LBOOL || k->params[p] == NT_TLBOOL ? "LubeNBool *, "
                        : k->loop ? "double *, " : "double, ");  /* (a loop's Numbers and lists: given back) */
        sb_puts(&c, k->ret == NT_VAL ? "Value *, int);\n" : "double *, int);\n");
        if (!k->ok_int) continue;
        sb_printf(&c, "static int64_t ni_%d(", i);
        for (int p = 0; p < k->d->nparams; p++) sb_puts(&c, "int64_t, ");
        sb_puts(&c, "int);\n");
    }
    sb_puts(&c, "\n");
    for (int i = 0; i < g.ncands; i++) {
        Cand *k = &g.cands[i];
        if (!k->ok) continue;
        gen_function(&g, k, 0, &c);
        if (k->ok_int) gen_function(&g, k, 1, &c);
        if (k->loop) sb_printf(&names, "%sthe loop on line %d", count++ ? ", " : "", k->d->line);
        else sb_printf(&names, "%s%s", count++ ? ", " : "", lube_shown(k->key));
        if (records_wrapper(&g, k, i, &c)) continue;
        /* the VM calls this: the whole-number version when it can, else (or when that gives up) the other */
        sb_printf(&c, "static int w_%d(const double *a, double *out, int left) {\n    (void)a;\n", i);
        if (k->ok_int) {
            sb_puts(&c, "    if (1");
            for (int p = 0; p < k->d->nparams; p++)
                if (k->params[p] == NT_NUM) sb_printf(&c, " && lube_nwhole(a[%d])", p);
            sb_printf(&c, ") {\n        int64_t r = ni_%d(", i);
            for (int p = 0; p < k->d->nparams; p++) sb_printf(&c, "(int64_t)a[%d], ", p);
            sb_puts(&c, "left);\n        if (r != LUBE_FAIL) {\n            *out = (double)r;\n            return 0;\n        }\n    }\n");
        }
        sb_printf(&c, "    return nf_%d(", i);
        for (int p = 0; p < k->d->nparams; p++) sb_printf(&c, "a[%d], ", p);
        sb_puts(&c, "out, left);\n}\n\n");
    }
    sb_puts(&c, "const LubeNative lube_natives[] = {\n");
    for (int i = 0; i < g.ncands; i++) {
        Cand *k = &g.cands[i];
        if (!k->ok) continue;
        int recs = 0;
        sb_printf(&c, "    {\"%s\", \"", k->key->chars);  /* (names are plain identifiers, :: in a package) */
        for (int p = 0; p < k->d->nparams; p++) {
            int t = k->params[p];
            sb_putc(&c, t == NT_NUM ? 'N' : t == NT_BOOL ? 'B' : t == NT_RLIST ? 'L' : t == NT_VAL ? 'V' : 'O');
            recs |= RECORD(t) || t == NT_VAL;
        }
        sb_printf(&c, ":%s\", 0x%llxull, ", k->ret == NT_NUM ? "N" : k->ret == NT_BOOL ? "B" : k->ret == NT_VAL ? "V" : "",
                  (unsigned long long)k->hash);
        if (k->ret == NT_VAL) sb_printf(&c, "0, 0, wv_%d},\n", i);
        else if (recs || k->loop) sb_printf(&c, "0, wv_%d, 0},\n", i);
        else sb_printf(&c, "w_%d, 0, 0},\n", i);
    }
    sb_printf(&c, "    {0, 0, 0, 0, 0, 0}\n};\nconst int lube_nnatives = %d;\n", count);

    int status = count ? 0 : -1;  /* -1: nothing to compile (a plain build will do) */
    char *srcdir = count ? find_sources(argv0) : NULL;
    if (count && !srcdir) {
        fprintf(stderr, "lube: --native needs Lube's C sources: set LUBE_SRC to the folder that has lube.h\n");
        status = 69;
    }
    StrBuf gen, cmd;
    sb_init(&gen);
    sb_init(&cmd);
    size_t base = strlen(exe_out);
    if (base > 4 && !strcmp(exe_out + base - 4, ".exe")) base -= 4;  /* (Windows) */
    sb_printf(&gen, "%.*s.c", (int)base, exe_out);
    FILE *f = status ? NULL : fopen(gen.data, "wb");
    if (!status && (!f || fwrite(c.data, 1, c.len, f) != c.len)) {
        fprintf(stderr, "lube: cannot write '%s'\n", gen.data);
        status = 73;
    }
    if (f) fclose(f);
    if (!status) {
        const char *cc = getenv("CC");
        const char *extra = getenv("LUBE_NATIVE_CFLAGS");
#ifdef _WIN32
        const char *dflt = "gcc";
        sb_putc(&cmd, '"');  /* (cmd.exe drops the outer quotes of a command that starts with one) */
#else
        const char *dflt = "cc";
#endif
        sb_printf(&cmd, "\"%s\" -O3 -std=c99 -ffp-contract=off -DLUBE_STACK_SIZE=0x%lx", cc && *cc ? cc : dflt,
                  (unsigned long)LUBE_STACK_SIZE);  /* (no a * b + c in one rounding: the VM's two) */
#if defined(__APPLE__)
        sb_printf(&cmd, " -Wl,-stack_size,0x%lx", (unsigned long)LUBE_STACK_SIZE);
#elif defined(_WIN32)
        sb_printf(&cmd, " -Wl,--stack,0x%lx", (unsigned long)LUBE_STACK_SIZE);
#endif
        StrBuf obj;  /* --fast: the machine code compiled apart, with its own flags (Lube's stay exact) */
        sb_init(&obj);
        if (fast) {
            StrBuf c1;
            sb_init(&c1);
            sb_printf(&obj, "%.*s.o", (int)base, exe_out);
#ifdef _WIN32
            sb_putc(&c1, '"');
#endif
            sb_printf(&c1, "\"%s\" -O3 -std=c99 -ffp-contract=fast -fassociative-math -fno-signed-zeros -fno-trapping-math"
                           " -fwrapv -I\"%s\" -c \"%s\" -o \"%s\"%s%s", cc && *cc ? cc : dflt, srcdir, gen.data, obj.data,
                      extra ? " " : "", extra ? extra : "");
#ifdef _WIN32
            sb_putc(&c1, '"');
#endif
            fflush(stdout);
            if (system(c1.data) != 0) status = 70;
            sb_free(&c1);
        }
        int n;
        char **files = os_files(srcdir, ".c", &n);
        for (int i = 0; i < n; i++) {
            if (strcmp(files[i], "nativetab.c")) sb_printf(&cmd, " \"%s/%s\"", srcdir, files[i]);
            free(files[i]);
        }
        free(files);
        sb_printf(&cmd, " \"%s\" -I\"%s\" -o \"%s\" -lm", fast ? obj.data : gen.data, srcdir, exe_out);
#ifndef _WIN32
        sb_puts(&cmd, " -lpthread");
#endif
        if (extra) sb_printf(&cmd, " %s", extra);
#ifdef _WIN32
        sb_putc(&cmd, '"');
#endif
        fflush(stdout);
        if (status || system(cmd.data) != 0) {
            fprintf(stderr, "lube: the C compiler (%s) failed\n", cc && *cc ? cc : dflt);
            status = 70;
        }
        if (!getenv("LUBE_NATIVE_KEEP")) remove(gen.data);  /* (LUBE_NATIVE_KEEP: to read it) */
        if (fast) remove(obj.data);
        sb_free(&obj);
    }
    if (status <= 0) printf("native: %s\n", count ? names.data : "none of its functions can be (a plain build)");
    StrBuf not;  /* the typed fns that are not, and where translating them stopped */
    sb_init(&not);
    for (int i = 0; i < g.ncands && status <= 0; i++) {
        Cand *k = &g.cands[i];
        int typed = k->ret != NT_NONE;  /* (a fn with no types at all is not expected to be native) */
        for (int p = 0; p < k->d->nparams; p++) typed |= k->d->types && k->d->types[p].set;
        if (!k->ok && typed) sb_printf(&not, "%s%s (line %d)", not.len ? ", " : "", lube_shown(k->key), k->why);
    }
    if (not.len) printf("not native: %s\n", not.data);
    sb_free(&not);
    free(srcdir);
    sb_free(&gen);
    sb_free(&cmd);
    sb_free(&c);
    sb_free(&names);
    for (int i = 0; i < nfiles; i++) arena_free(arenas[i]);
    free(arenas);
    free(trees);
    for (int i = 0; i < g.ncands; i++) {  /* (a loop's made-up fn) */
        if (!g.cands[i].loop) continue;
        FuncDecl *d = g.cands[i].d;
        free(d->body->as.block.stmts.items);
        free(d->body);
        free(d->params);
        free(d->modes);
        free(d->defaults);
        free(d);
    }
    free(g.cands);
    release(OBJ_VAL(g.declared));
    release(OBJ_VAL(g.vars));
    free(g.dkeys);
    free(g.dtypes);
    free(g.dshape);
    free(g.dneg);
    free(g.tnn);
    free(g.shapes);
    free(g.classes);
    return status;
}

/* ---------- records: copying an object's Number fields in and out ---------- */

/* Where field `name` (interned) is in m's entries, trying `hint` first; -1 if not there. */
static int rec_find(ObjMap *m, ObjString *name, int hint) {
    if (hint >= 0 && (size_t)hint < m->used && m->entries[hint].live && m->entries[hint].key.as.obj == (Obj *)name)
        return hint;
    for (size_t e = 0; e < m->used; e++)
        if (m->entries[e].live && IS_OBJ(m->entries[e].key) && m->entries[e].key.as.obj == (Obj *)name) return (int)e;
    return -1;
}

/* The fields of object v into out (an instance's places cached by class). 1 if one is missing or not a Number. */
int lube_rec_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, const char *cls, double *out) {
    if (c->I != I) {  /* (each Interp has interned names of its own) */
        for (int f = 0; f < nf; f++) c->names[f] = lube_intern(I, fields[f], strlen(fields[f]));
        c->I = I;
        c->klass = c->exact = NULL;
    }
    if (!IS_MAP(v)) return 1;
    ObjMap *m = AS_MAP(v);
    if (cls && (!m->klass || m->klass != c->exact)) {  /* (exactly that class: its methods are called on it) */
        if (!m->klass || strcmp(m->klass->key->chars, cls)) return 1;
        c->exact = m->klass;
    }
    int same = m->klass && m->klass == c->klass;
    for (int f = 0; f < nf; f++) {
        int e = rec_find(m, c->names[f], same ? c->pos[f] : -1);
        if (e < 0 || !IS_NUM(m->entries[e].value)) return 1;
        out[f] = m->entries[e].value.as.number;
        c->pos[f] = e;
    }
    c->klass = m->klass;
    return 0;
}

/* The records of list v into *out (*n of them, nf numbers each): buf, when
 * they fit in its bufsize numbers, else a new array. 1 if one is not a record. */
int lube_reclist_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, const char *cls, double **out,
                      int64_t *n, double *buf, size_t bufsize) {
    *out = buf;
    if (!IS_LIST(v)) return 1;
    ObjList *l = AS_LIST(v);
    size_t want = l->count * (size_t)nf;
    if (want > bufsize) *out = xmalloc(sizeof(double) * want);
    *n = (int64_t)l->count;
    for (size_t i = 0; i < l->count; i++)
        if (lube_rec_load(I, c, l->items[i], fields, nf, cls, *out + i * (size_t)nf)) return 1;
    return 0;
}

/* The object in *slot, whose fields were loaded as orig, takes the ones
 * that differ in vals (it is copied first if shared). (Nothing ran in
 * between: its fields are still orig, where they were.) */
void lube_rec_store(Interp *I, LubeRecCache *c, Value *slot, int nf, const double *vals, const double *orig) {
    (void)I;
    if (!memcmp(orig, vals, sizeof(double) * (size_t)nf)) return;
    make_unique(slot);
    ObjMap *m = AS_MAP(*slot);
    for (int f = 0; f < nf; f++)
        if (memcmp(&orig[f], &vals[f], sizeof(double))) m->entries[rec_find(m, c->names[f], c->pos[f])].value = NUM_VAL(vals[f]);
}

/* The records of the list in *list (loaded as orig) that changed in vals, written back (the list copied first if shared). */
void lube_reclist_store(Interp *I, LubeRecCache *c, Value *list, int nf, const double *vals, const double *orig) {
    ObjList *l = AS_LIST(*list);
    int unique = 0;
    for (size_t i = 0; i < l->count; i++) {
        const double *v = vals + i * (size_t)nf, *o = orig + i * (size_t)nf;
        if (!memcmp(v, o, sizeof(double) * (size_t)nf)) continue;
        if (!unique) {  /* (the list first, if it is shared) */
            make_unique(list);
            l = AS_LIST(*list);
            unique = 1;
        }
        lube_rec_store(I, c, &l->items[i], nf, v, o);
    }
}

/* A global a loop's machine code uses (written: it may change it): NULL
 * when it is not there yet, is lent, or (written) an `on` block watches it. */
Value *lube_native_global(Interp *I, const char *name, int written) {
    ObjString *s = lube_intern(I, name, strlen(name));
    Value *v = map_find(I->globals, OBJ_VAL(s));
    if (!v || v->type == VAL_LENT || (written && lube_global_watched(I, s))) return NULL;
    return v;
}

/* The math built-ins machine code calls: here, apart from it, so that its C
 * compiler cannot work them out itself (for a constant) or rewrite them
 * (pow(8, y) as exp2(3 * y)), which would not always give the VM's bits. */
double lube_m_sin(double x) { return sin(x); }
double lube_m_cos(double x) { return cos(x); }
double lube_m_tan(double x) { return tan(x); }
double lube_m_exp(double x) { return exp(x); }
double lube_m_log(double x) { return log(x); }
double lube_m_pow(double x, double y) { return pow(x, y); }
double lube_m_atan2(double y, double x) { return atan2(y, x); }

/* A new list of the n values in items (it takes them over). */
Value lube_nv_list(int n, const Value *items) {
    ObjList *l = new_list();
    list_reserve(l, (size_t)n);
    for (int i = 0; i < n; i++) l->items[i] = items[i];
    l->count = (size_t)n;
    return OBJ_VAL(l);
}

/* A loop's global list (of Numbers, or bools: of Bools; typed: Number[] or
 * Bool[], else untyped) into a new array *a, of *n: 1 if it is not one. */
int lube_numlist_load(Value v, int bools, int typed, void **a, int64_t *n) {
    if (!IS_LIST(v) || AS_LIST(v)->elem != (typed ? (bools ? TT_BOOL : TT_NUMBER) : TT_ANY)) return 1;
    ObjList *l = AS_LIST(v);
    double *d = bools ? NULL : xmalloc(sizeof(double) * (l->count ? l->count : 1));
    unsigned char *b = bools ? xmalloc(l->count ? l->count : 1) : NULL;
    for (size_t i = 0; i < l->count; i++) {
        if (bools ? !IS_BOOL(l->items[i]) : !IS_NUM(l->items[i])) {
            free(d);
            free(b);
            return 1;
        }
        if (bools) b[i] = (unsigned char)l->items[i].as.boolean;
        else d[i] = l->items[i].as.number;
    }
    *a = bools ? (void *)b : (void *)d;
    *n = (int64_t)l->count;
    return 0;
}

/* The list in *slot (copied first if shared) takes the n items of array a, which it frees. */
void lube_numlist_store(Value *slot, int bools, void *a, int64_t n) {
    make_unique(slot);
    ObjList *l = AS_LIST(*slot);
    list_reserve(l, (size_t)n);
    for (int64_t i = 0; i < n; i++)
        l->items[i] = bools ? BOOL_VAL(((unsigned char *)a)[i]) : NUM_VAL(((double *)a)[i]);
    l->count = (size_t)n;
    free(a);
}

/* Top-level statement `index` of src: the machine code this executable has
 * for it (a loop, compiled from this very text), or -1. */
int lube_native_loop(Source *src, int index) {
    char key[32];
    snprintf(key, sizeof key, "@%d", index);
    for (int k = 0; k < lube_nnatives; k++)
        if (!strcmp(lube_natives[k].key, key) && lube_natives[k].src_hash == lube_hash_text(src->text, src->length))
            return k;
    return -1;
}

/* After a source is compiled: its top-level fns that this executable has
 * machine code for (compiled from this very text) use it. */
void lube_native_attach(Interp *I, FuncDecl *program, Source *src) {
    (void)I;
    if (!lube_nnatives) return;
    uint64_t hash = lube_hash_text(src->text, src->length);
    for (int i = 0; i < program->code.nprotos; i++) {
        FuncDecl *d = program->code.protos[i];
        for (int k = 0; d->key && k < lube_nnatives; k++)
            if (lube_natives[k].src_hash == hash && !strcmp(lube_natives[k].key, d->key->chars) &&
                (int)strcspn(lube_natives[k].sig, ":") == d->nparams)
                d->native = &lube_natives[k];
    }
    for (int i = 0; i < program->code.nclasses; i++) {  /* methods: Class.name */
        ClassDesc *cd = &program->code.classes[i];
        size_t n = strlen(cd->key->chars);
        for (int m = 0; m < cd->nmethods; m++) {
            FuncDecl *d = cd->methods[m];
            for (int k = 0; k < lube_nnatives; k++) {
                const char *key = lube_natives[k].key;
                if (lube_natives[k].src_hash == hash && !strncmp(key, cd->key->chars, n) && key[n] == '.' &&
                    !strcmp(key + n + 1, d->name->chars) && (int)strcspn(lube_natives[k].sig, ":") == d->nparams)
                    d->native = &lube_natives[k];
            }
        }
    }
}
