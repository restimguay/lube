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
       NT_RLIST, NT_REC };
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
} Cand;

typedef struct {
    Cand *cands;
    int ncands;
    ObjMap *declared;   /* every global the program's files declare: key -> how many times */
    Cand *cur;          /* the fn being translated */
    int mode;           /* 0: its double version; 1: its whole-number version */
    StrBuf *out;
    int tmp;            /* temporaries: t0, t1, ... */
    /* types belong to declarations (a var, a let, a loop's variable, a parameter: keyed by its VarRef or
     * name), over the whole fn (see gen_function); live[slot]: the declaration a slot holds now (-1: none) */
    void **dkeys;
    int *dtypes, ndecls, capdecls;
    int *dshape;        /* a record declaration's shape */
    int *live;
    /* shapes: the fields a record parameter's uses need, found as it is translated */
    struct { ObjString *fields[LUBE_REC_MAX]; int n; } *shapes;
    int nshapes;
    int widened;        /* a slot's type changed in this pass: translate again */
    int failed;
    int line;           /* of the code being translated (failed: where it stopped) */
} Gen;

static int gen_expr(Gen *g, Node *n, char *res);
static void gen_stmt(Gen *g, Node *n);

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
static int new_tmp(Gen *g) { return g->tmp++; }
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
        }
        g->dkeys[i] = key;
        g->dtypes[i] = 0;
        g->dshape[i] = -1;
        g->ndecls++;
    }
    g->live[slot] = i;
}

/* The type slot's variable holds (0: none yet). */
static int type_of(Gen *g, int slot) { return g->live[slot] >= 0 ? g->dtypes[g->live[slot]] : 0; }

/* n is a record parameter, or a record taken out of one, of kind `want`
 * (NT_RLIST or NT_REC): its slot, + 1; else 0. An untyped parameter takes
 * the kind of its first use. */
static int record(Gen *g, Node *n, int want) {
    if (n->kind != N_VAR || n->as.var.kind != VAR_LOCAL || g->live[n->as.var.index] < 0) return 0;
    int d = g->live[n->as.var.index], *t = &g->dtypes[d];
    if (*t == UNTYPED) {  /* (its parameter now has a kind: the fn's signature changes) */
        *t = want;
        g->cur->params[n->as.var.index] = want;
        g->widened = 1;
    }
    return *t == want && g->dshape[d] >= 0 ? n->as.var.index + 1 : 0;
}

/* Where field `name` is in the record in slot s (added to its shape when new). */
static int field_of(Gen *g, int s, ObjString *name) {
    int sh = g->dshape[g->live[s]], f = 0;
    while (f < g->shapes[sh].n && g->shapes[sh].fields[f] != name) f++;
    if (f == g->shapes[sh].n) {
        if (f == LUBE_REC_MAX) return fail(g) - 1;
        g->shapes[sh].fields[g->shapes[sh].n++] = name;
        g->widened = 1;  /* (the rows are wider: translate again) */
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
    snprintf(place, 64, "R%d + k%d * %d", s, k, g->shapes[g->dshape[g->live[s]]].n);
    return 1;
}

/* Slot s (just declared: let r = xs[i], or a loop's) becomes a copy of the
 * record at C address `from`, of shape sh. (Nothing else gives that
 * declaration a type: it holds this record kind only.) */
static void record_copy(Gen *g, int s, int sh, const char *from) {
    int d = g->live[s];
    g->dtypes[d] = NT_REC;
    g->dshape[d] = sh;
    sb_printf(g->out, "memcpy(Bs%d, %s, sizeof(double) * %d);\nBp%d = Bs%d;\n", s, from, g->shapes[sh].n, s, s);
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
    if (st < 0) return fail(g);  /* (a record parameter given another value) */
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
    if (st) sb_printf(g->out, "%c%d = %s;\n", letter(st), slot, how);
}

/* ---------- expressions: each leaves its value in a C expression (res) ---------- */


static void number_literal(char *res, double x) {  /* (in parentheses: -x of -2.5 is not --2.5) */
    snprintf(res, 64, "(%.17g", x);
    if (!strpbrk(res, ".en")) strcat(res, ".0");  /* a double, never an int (1/2 is not 0) */
    strcat(res, ")");
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
        sb_printf(g->out, "int64_t t%d = %s %c %s;\nif (LUBE_OUT(t%d)) %s\n", t, a, op == T_PLUS ? '+' : '-', b, t, bail(g));
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_STAR) {  /* (as a double: exact when it fits; never -0) */
        sb_printf(g->out, "double m%d = (double)%s * (double)%s;\n", t, a, b);
        sb_printf(g->out, "if (m%d > LUBE_BIGD || m%d < -LUBE_BIGD || (m%d == 0 && (%s < 0 || %s < 0))) %s\n", t, t,
                  t, a, b, bail(g));
        sb_printf(g->out, "int64_t t%d = (int64_t)m%d;\n", t, t);
        snprintf(res, 64, "t%d", t);
        return NT_INT;
    } else if (ta == NT_INT && tb == NT_INT && op == T_PERCENT) {  /* Lube's %: the sign of b; never -0 */
        sb_printf(g->out, "if (%s == 0) %s\nint64_t t%d = %s %% %s;\n", b, bail(g), t, a, b);
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
            sb_printf(g->out, "double t%d = pow(%s, %s);\n", t, x, y);
            snprintf(res, 64, "t%d", t);
            return NT_NUM;
        } else {
            return fail(g);
        }
    }
    snprintf(res, 64, "t%d", t);
    return NT_BOOL;
}

/* n is a local list of the fn's own: its type (NT_LNUM, NT_LBOOL); else 0. */
static int list_slot(Gen *g, Node *n) {
    int t = n->kind == N_VAR && n->as.var.kind == VAR_LOCAL ? type_of(g, n->as.var.index) : 0;
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
    static const char *const one[][2] = {{"sqrt", "sqrt"}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"},
                                         {"exp", "exp"}, {"floor", "floor"}, {"ceil", "ceil"}, {"abs", "fabs"},
                                         {"log", "log"}, {"round", "lube_nround"}};
    static const char *const two[][2] = {{"pow", "pow"}, {"atan2", "atan2"}, {"log", "lube_nlog2"}};
    for (size_t i = 0; nargs == 1 && i < sizeof one / sizeof one[0]; i++)
        if (!strcmp(name->chars, one[i][0])) return one[i][1];
    for (size_t i = 0; nargs == 2 && i < sizeof two / sizeof two[0]; i++)
        if (!strcmp(name->chars, two[i][0])) return two[i][1];
    return NULL;
}

/* A call; need: its value is used. */
static int gen_call(Gen *g, Node *n, char *res, int need) {
    Node *callee = n->as.call.callee;
    NodeList *args = &n->as.call.args;
    char a[NATIVE_MAX_PARAMS][64], x[2][64];
    int ta[NATIVE_MAX_PARAMS];
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
    if (is_builtin(g, callee, "push") && args->count == 2 && args->items[0]->kind == N_REF && !need) {
        gen_push(g, args->items[0]->as.expr, args->items[1]);
        return g->failed ? 0 : NT_NUM;
    }
    VarRef *v = &callee->as.var;
    Cand *c = v->kind == VAR_SELF ? g->cur : v->kind == VAR_GLOBAL ? cand_for(g, v->name) : NULL;
    const char *fn = !c && v->kind == VAR_GLOBAL && !map_get(g->declared, OBJ_VAL(v->name), NULL)
                         ? builtin(v->name, args->count) : NULL;
    if (!c && !fn) return fail(g);
    int whole = 1;  /* every Number argument is whole: the callee's whole-number version can take them */
    for (int i = 0; i < args->count; i++) {
        ta[i] = gen_expr(g, args->items[i], a[i]);
        int want = !c ? NT_NUM : i < c->d->nparams ? c->params[i] : -1;  /* (records are not passed on: -1, 8, 9) */
        if (want < 0 || RECORD(want)) return fail(g);
        if (!ta[i] || (want == NT_NUM ? !NUMERIC(ta[i]) : ta[i] != want)) return fail(g);
        whole &= ta[i] != NT_NUM;
    }
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
        if (args->count > 1) as_double(ta[1], a[1], x[1]);
        sb_printf(g->out, "double t%d = %s(%s%s%s);\n", t, fn, x[0], args->count > 1 ? ", " : "", args->count > 1 ? x[1] : "");
        snprintf(res, 64, "t%d", t);
        return NT_NUM;
    }
    if (args->count != c->d->nparams || (need && c->ret == NT_NONE)) return fail(g);
    int k = (int)(c - g->cands);
    if (whole && c->ok_int) {  /* its whole-number version */
        sb_printf(g->out, "int64_t t%d = ni_%d(", t, k);
        for (int i = 0; i < args->count; i++) sb_printf(g->out, "%s, ", a[i]);
        sb_printf(g->out, "left - 1);\nif (t%d == LUBE_FAIL) %s\n", t, bail(g));
        snprintf(res, 64, c->ret == NT_BOOL ? "((int)t%d)" : "t%d", t);
        return c->ret == NT_BOOL ? NT_BOOL : NT_INT;
    }
    sb_printf(g->out, "double t%d;\nif (nf_%d(", t, k);
    for (int i = 0; i < args->count; i++) {
        as_double(ta[i], a[i], x[0]);
        sb_printf(g->out, "%s, ", x[0]);
    }
    sb_printf(g->out, "&t%d, left - 1)) %s\n", t, bail(g));
    snprintf(res, 64, c->ret == NT_BOOL ? "(t%d != 0)" : "t%d", t);
    return c->ret == NT_BOOL ? NT_BOOL : NT_NUM;
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
        if (t <= 0 || t >= NT_LNUM) return fail(g);  /* (a list or record is not a value here: it never leaves the fn) */
        snprintf(res, 64, "%c%d", letter(t), n->as.var.index);
        return t;
    }
    case N_UNARY: {
        int t = gen_expr(g, n->as.unary.operand, a), k = new_tmp(g);
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
        int tb = ta ? gen_expr(g, n->as.binary.right, b) : 0;
        if (!ta || !tb) return fail(g);
        return gen_binop(g, n->as.binary.op, ta, a, tb, b, res);
    }
    case N_AND:
    case N_OR: {  /* both Bools; the right one only runs when it decides */
        if (gen_expr(g, n->as.binary.left, a) != NT_BOOL) return fail(g);
        int k = new_tmp(g);
        sb_printf(g->out, "int t%d = %s;\nif (%st%d) {\n", k, a, n->kind == N_AND ? "" : "!", k);
        if (gen_expr(g, n->as.binary.right, b) != NT_BOOL) return fail(g);
        sb_printf(g->out, "t%d = %s;\n}\n", k, b);
        snprintf(res, 64, "t%d", k);
        return NT_BOOL;
    }
    case N_CALL:
        return gen_call(g, n, res, 1);
    case N_FIELD: {  /* xs[i].x, r.x: a record's field */
        char p[64];
        if (!field_place(g, n, p)) return fail(g);
        int t = new_tmp(g);
        sb_printf(g->out, "double t%d = %s;\n", t, p);
        snprintf(res, 64, "t%d", t);
        return NT_NUM;
    }
    case N_INDEX: {  /* xs[i], xs a list of the fn's own */
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
    if (op == T_ASSIGN) {
        store(g, slot, t, v);
        return;
    }
    TokenType bin = op == T_PLUS_EQ ? T_PLUS : op == T_MINUS_EQ ? T_MINUS : op == T_STAR_EQ ? T_STAR
                  : op == T_SLASH_EQ ? T_SLASH : T_PERCENT;
    snprintf(cur, sizeof cur, "%c%d", letter(st), slot);
    int rt = st && NUMERIC(st) && NUMERIC(t) ? gen_binop(g, bin, st, cur, t, v, r) : fail(g);
    if (rt) store(g, slot, rt, r);
}

/* Does code n (statements) change something in slot s (s = .., s.x = .., s[i].x = ..)? */
static int writes_to(Node *n, int s) {
    if (!n) return 0;
    switch (n->kind) {
    case N_BLOCK:
        for (int i = 0; i < n->as.block.stmts.count; i++)
            if (writes_to(n->as.block.stmts.items[i], s)) return 1;
        return 0;
    case N_IF:
        return writes_to(n->as.if_s.then_branch, s) || writes_to(n->as.if_s.else_branch, s);
    case N_WHILE:
        return writes_to(n->as.while_s.body, s);
    case N_FOR:
        return writes_to(n->as.for_s.body, s);
    case N_EXPR: {
        Node *t = n->as.expr->kind == N_ASSIGN ? n->as.expr->as.assign.target : NULL;
        while (t && (t->kind == N_INDEX || t->kind == N_FIELD)) t = t->kind == N_INDEX ? t->as.index.object : t->as.field.object;
        return t && t->kind == N_VAR && t->as.var.kind == VAR_LOCAL && t->as.var.index == s;
    }
    default:
        return 0;
    }
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
        int t = n->as.let.is_fn ? 0 : gen_expr(g, n->as.let.value, v);
        if (t) store(g, n->as.let.var.index, t, v);
        else fail(g);
    } break;
    case N_BLOCK:
        sb_puts(g->out, "{\n");
        gen_block(g, &n->as.block.stmts);
        sb_puts(g->out, "}\n");
        break;
    case N_IF:
        if (gen_expr(g, n->as.if_s.cond, v) != NT_BOOL) {
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
    case N_WHILE:  /* the condition again on each turn (and after continue) */
        sb_puts(g->out, "for (;;) {\n");
        if (gen_expr(g, n->as.while_s.cond, v) != NT_BOOL) {
            fail(g);
            break;
        }
        sb_printf(g->out, "if (!%s) break;\n", v);
        gen_stmt(g, n->as.while_s.body);
        sb_puts(g->out, "}\n");
        break;
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
            snprintf(x, sizeof x, "R%d + k%d * %d", s, k, g->shapes[sh].n);
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
        int k = new_tmp(g);
        as_double(ts, v, x);
        as_double(te, e, y);
        sb_printf(g->out, "{\n%s s%d = %s;\ndouble n%d = lube_nrange(%s, %s, %d);\n", ctype(ts), k, v, k, x, y,
                  it->as.range.inclusive);
        if (ts == NT_INT) sb_printf(g->out, "if ((double)s%d + n%d > LUBE_BIGD) %s\n", k, k, bail(g));
        sb_printf(g->out, "for (int64_t k%d = 0; (double)k%d < n%d; k%d++) {\n", k, k, k, k);
        snprintf(x, sizeof x, ts == NT_INT ? "s%d + k%d" : "s%d + (double)k%d", k, k);
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
        break;
    }
    case N_RETURN: {
        int want = g->cur->ret, t = n->as.expr ? gen_expr(g, n->as.expr, v) : NT_NONE;
        if (n->as.expr && !t) break;
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
    g->widened = 0;
    g->line = d->line;  /* (where it stops, if before any of its code) */
    for (int i = 0; i < nslots; i++) g->live[i] = -1;
    if (g->mode) sb_printf(out, "static int64_t ni_%d(", k);
    else sb_printf(out, "static int nf_%d(", k);
    for (int i = 0; i < d->nparams; i++) {
        int t = c->params[i];
        if (t == NT_REC) sb_printf(out, "double *r%d, ", i);
        else if (t == UNTYPED || t == NT_RLIST) sb_printf(out, "double *r%d, int64_t rn%d, ", i, i);
        else sb_printf(out, "%s p%d, ", g->mode ? "int64_t" : "double", i);
    }
    /* one way out (done:), where the lists are let go; R: the result, or that it gave up */
    sb_printf(out, "%sint left) {\n%s R = %s;\n", g->mode ? "" : "double *out, ", g->mode ? "int64_t" : "int",
              g->mode ? "LUBE_FAIL" : "1");
    sb_puts(out, "LubeNNum Lnt = {0, 0, 0};\nLubeNBool Lbt = {0, 0, 0};\n");
    for (int i = 0; i < nslots; i++)
        sb_printf(out, "double d%d = 0; int64_t i%d = 0; int b%d = 0; LubeNNum Ln%d = {0, 0, 0}; LubeNBool Lb%d = {0, 0, 0};\n"
                       "double *R%d = 0; int64_t Rn%d = 0; double *Bp%d = 0; double Bs%d[LUBE_REC_MAX];\n"
                       "(void)d%d; (void)i%d; (void)b%d; (void)R%d; (void)Rn%d; (void)Bp%d; (void)Bs%d;\n",
                  i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
    sb_puts(out, "if (left <= 0) goto done;\n");
    for (int i = 0; i < d->nparams; i++) {
        char p[64];
        int t = c->params[i];
        declare(g, &d->params[i], i);
        if (t == UNTYPED || RECORD(t)) {  /* a record parameter: the wrapper's copy of its fields */
            g->dtypes[g->live[i]] = t;
            g->dshape[g->live[i]] = c->shape[i];
            if (t == NT_REC) sb_printf(out, "Bp%d = r%d;\n", i, i);
            else sb_printf(out, "R%d = r%d;\nRn%d = rn%d;\n", i, i, i, i);
            continue;
        }
        snprintf(p, sizeof p, t == NT_BOOL ? "(p%d != 0)" : "p%d", i);
        store(g, i, t == NT_NUM && g->mode ? NT_INT : t, p);
    }
    if (d->is_arrow) {  /* fn f(x: Number): Number => x * x */
        Node ret;
        memset(&ret, 0, sizeof ret);
        ret.kind = N_RETURN;
        ret.as.expr = d->body;
        if (c->ret == NT_NONE) fail(g);
        else gen_stmt(g, &ret);
    } else {
        gen_block(g, &d->body->as.block.stmts);
    }
    /* (fell off the end: nothing to return, or the VM's error that the value is missing) */
    if (c->ret == NT_NONE) sb_puts(out, "R = 0;\n");
    sb_puts(out, "done:\nfree(Lnt.a);\nfree(Lbt.a);\n");
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
    return !g->failed;
}

/* ---------- the program ---------- */

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

static void note_declared(Gen *g, Node *s) {
    ObjString *key = s->kind == N_LET ? s->as.let.var.name : s->kind == N_CLASS ? s->as.klass.var.name
                   : s->kind == N_IMPORT ? s->as.import.bind : NULL;
    if (!key) return;
    Value v;
    double n = map_get(g->declared, OBJ_VAL(key), &v) ? v.as.number : 0;
    map_set(g->declared, retain(OBJ_VAL(key)), NUM_VAL(n + 1));
}

/* For a fn with record parameters: the wrapper the VM calls with the
 * arguments as they are. It copies each record's fields into an array
 * (gives up, to the VM, when an object lacks one or it is not a Number),
 * calls the machine code, and writes back to & parameters the fields that
 * changed. Returns 0 when the fn has no record parameter. */
static int records_wrapper(Gen *g, Cand *k, int i, StrBuf *c) {
    int recs = 0;
    for (int p = 0; p < k->d->nparams; p++) recs |= RECORD(k->params[p]);
    if (!recs) return 0;
    for (int p = 0; p < k->d->nparams; p++) {
        if (!RECORD(k->params[p])) continue;
        int sh = k->shape[p];
        sb_printf(c, "static const char *const fs_%d_%d[] = {", i, p);
        for (int f = 0; f < g->shapes[sh].n; f++) sb_printf(c, "\"%s\", ", g->shapes[sh].fields[f]->chars);
        sb_printf(c, "0};\nstatic LUBE_THREAD_LOCAL LubeRecCache rc_%d_%d;\n", i, p);
    }
    sb_printf(c, "static int wv_%d(Interp *I, Value *args, double *out, int left) {\n    int R = 1;\n", i);
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p];
        if (t == NT_RLIST)
            sb_printf(c, "    double rs%d[256], os%d[256], *r%d = rs%d, *o%d = os%d;\n    int64_t rn%d = 0;\n", p, p, p, p, p, p, p);
        else if (t == NT_REC) sb_printf(c, "    double rb%d[LUBE_REC_MAX], ob%d[LUBE_REC_MAX];\n", p, p);
        else sb_printf(c, "    double a%d = 0;\n", p);
    }
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p], nf = RECORD(t) ? g->shapes[k->shape[p]].n : 0;
        int ref = k->d->modes[p] == PARAM_REF;  /* (& : keep what was loaded, to see what changed) */
        if (t == NT_RLIST) {
            sb_printf(c, "    if (lube_reclist_load(I, &rc_%d_%d, args[%d], fs_%d_%d, %d, &r%d, &rn%d, rs%d, 256)) goto done;\n", i, p,
                      p, i, p, nf, p, p, p);
            if (ref) sb_printf(c, "    if (r%d != rs%d) o%d = malloc(sizeof(double) * (size_t)(rn%d * %d + 1));\n"
                                  "    if (!o%d) goto done;\n    memcpy(o%d, r%d, sizeof(double) * (size_t)(rn%d * %d));\n",
                               p, p, p, p, nf, p, p, p, p, nf);
        } else if (t == NT_REC) {
            sb_printf(c, "    if (lube_rec_load(I, &rc_%d_%d, args[%d], fs_%d_%d, %d, rb%d)) goto done;\n", i, p, p, i, p, nf, p);
            if (ref) sb_printf(c, "    memcpy(ob%d, rb%d, sizeof(double) * %d);\n", p, p, nf);
        }
        else
            sb_printf(c, "    if (!IS_%s(args[%d])) goto done;\n    a%d = args[%d].as.%s;\n", t == NT_NUM ? "NUM" : "BOOL", p, p, p,
                      t == NT_NUM ? "number" : "boolean");
    }
    sb_printf(c, "    R = nf_%d(", i);
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p];
        if (t == NT_RLIST) sb_printf(c, "r%d, rn%d, ", p, p);
        else if (t == NT_REC) sb_printf(c, "rb%d, ", p);
        else sb_printf(c, "a%d, ", p);
    }
    sb_puts(c, "out, left);\n    if (R == 0) {\n");
    for (int p = 0; p < k->d->nparams; p++) {
        int t = k->params[p], nf = RECORD(t) ? g->shapes[k->shape[p]].n : 0;
        if (!RECORD(t) || k->d->modes[p] != PARAM_REF) continue;  /* (& parameters only: the others were copies) */
        if (t == NT_RLIST)
            sb_printf(c, "        lube_reclist_store(I, &rc_%d_%d, &args[%d], %d, r%d, o%d);\n", i, p, p, nf, p, p);
        else
            sb_printf(c, "        lube_rec_store(I, &rc_%d_%d, &args[%d], %d, rb%d, ob%d);\n", i, p, p, nf, p, p);
    }
    sb_puts(c, "    }\ndone:\n");
    for (int p = 0; p < k->d->nparams; p++)
        if (k->params[p] == NT_RLIST) sb_printf(c, "    if (r%d != rs%d) free(r%d);\n    if (o%d != os%d) free(o%d);\n", p, p, p, p, p, p);
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
    "static double lube_nround(double x) { return floor(x + 0.5); }\n"
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
    "static double lube_nlog2(double x, double base) { return log(x) / log(base); }\n\n",
    NULL};

/* Compiles the program (its files: keys and texts, read from dir) into the
 * executable exe_out: Lube itself, with the functions that can be native
 * compiled to machine code. Returns 0, or an exit status after a message. */
int lube_native_build(Interp *I, int nfiles, char **keys, char **texts, size_t *lens, const char *argv0,
                      const char *exe_out) {
    Gen g;
    memset(&g, 0, sizeof g);
    g.declared = new_map();
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
    for (int i = 0; i < g.ncands; i++) {  /* a global declared twice may not be this fn when it is called */
        Value n;
        map_get(g.declared, OBJ_VAL(g.cands[i].key), &n);
        if (n.as.number > 1) g.cands[i].ok = 0;
    }
    StrBuf body;
    int changed = 1;
    while (changed) {  /* drop the versions that cannot be, until the rest only call each other */
        changed = 0;
        for (int i = 0; i < g.ncands; i++) {
            Cand *k = &g.cands[i];
            for (int mode = 0; mode < 2; mode++) {
                if (!(mode ? k->ok_int : k->ok)) continue;
                sb_init(&body);
                if (!gen_function(&g, k, mode, &body)) {
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
            sb_puts(&c, k->params[p] == NT_REC ? "double *, " : k->params[p] == NT_RLIST ? "double *, int64_t, " : "double, ");
        sb_puts(&c, "double *, int);\n");
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
        sb_printf(&names, "%s%s", count++ ? ", " : "", lube_shown(k->key));
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
            sb_putc(&c, t == NT_NUM ? 'N' : t == NT_BOOL ? 'B' : t == NT_RLIST ? 'L' : 'O');
            recs |= RECORD(t);
        }
        sb_printf(&c, ":%s\", 0x%llxull, ", k->ret == NT_NUM ? "N" : k->ret == NT_BOOL ? "B" : "", (unsigned long long)k->hash);
        if (recs) sb_printf(&c, "0, wv_%d},\n", i);
        else sb_printf(&c, "w_%d, 0},\n", i);
    }
    sb_printf(&c, "    {0, 0, 0, 0, 0}\n};\nconst int lube_nnatives = %d;\n", count);

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
        sb_printf(&cmd, "\"%s\" -O3 -std=c99 -DLUBE_STACK_SIZE=0x%lx", cc && *cc ? cc : dflt, (unsigned long)LUBE_STACK_SIZE);
#if defined(__APPLE__)
        sb_printf(&cmd, " -Wl,-stack_size,0x%lx", (unsigned long)LUBE_STACK_SIZE);
#elif defined(_WIN32)
        sb_printf(&cmd, " -Wl,--stack,0x%lx", (unsigned long)LUBE_STACK_SIZE);
#endif
        int n;
        char **files = os_files(srcdir, ".c", &n);
        for (int i = 0; i < n; i++) {
            if (strcmp(files[i], "nativetab.c")) sb_printf(&cmd, " \"%s/%s\"", srcdir, files[i]);
            free(files[i]);
        }
        free(files);
        sb_printf(&cmd, " \"%s\" -I\"%s\" -o \"%s\" -lm", gen.data, srcdir, exe_out);
#ifndef _WIN32
        sb_puts(&cmd, " -lpthread");
#endif
        if (extra) sb_printf(&cmd, " %s", extra);
#ifdef _WIN32
        sb_putc(&cmd, '"');
#endif
        fflush(stdout);
        if (system(cmd.data) != 0) {
            fprintf(stderr, "lube: the C compiler (%s) failed\n", cc && *cc ? cc : dflt);
            status = 70;
        }
        if (!getenv("LUBE_NATIVE_KEEP")) remove(gen.data);  /* (LUBE_NATIVE_KEEP: to read it) */
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
    free(g.cands);
    release(OBJ_VAL(g.declared));
    free(g.dkeys);
    free(g.dtypes);
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
int lube_rec_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, double *out) {
    if (c->I != I) {  /* (each Interp has interned names of its own) */
        for (int f = 0; f < nf; f++) c->names[f] = lube_intern(I, fields[f], strlen(fields[f]));
        c->I = I;
        c->klass = NULL;
    }
    if (!IS_MAP(v)) return 1;
    ObjMap *m = AS_MAP(v);
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
int lube_reclist_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, double **out, int64_t *n,
                      double *buf, size_t bufsize) {
    *out = buf;
    if (!IS_LIST(v)) return 1;
    ObjList *l = AS_LIST(v);
    size_t want = l->count * (size_t)nf;
    if (want > bufsize) *out = xmalloc(sizeof(double) * want);
    *n = (int64_t)l->count;
    for (size_t i = 0; i < l->count; i++)
        if (lube_rec_load(I, c, l->items[i], fields, nf, *out + i * (size_t)nf)) return 1;
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
}
