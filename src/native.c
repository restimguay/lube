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
 * any Number (a double), NT_NONE a fn's result when it returns nothing. */
enum { NT_NONE, NT_NUM, NT_BOOL, NT_INT };
#define NUMERIC(t) ((t) == NT_NUM || (t) == NT_INT)
#define NATIVE_MAX_PARAMS 8

typedef struct {
    FuncDecl *d;
    ObjString *key;     /* the fn's global */
    uint64_t hash;      /* of its file's text */
    int params[NATIVE_MAX_PARAMS], ret;
    int ok;             /* native: its double version compiles (a fixpoint: callees must be native too) */
    int ok_int;         /* ...and its whole-number version */
} Cand;

typedef struct {
    Cand *cands;
    int ncands;
    ObjMap *declared;   /* every global the program's files declare: key -> how many times */
    Cand *cur;          /* the fn being translated */
    int mode;           /* 0: its double version; 1: its whole-number version */
    StrBuf *out;
    int tmp;            /* temporaries: t0, t1, ... */
    int *slots;         /* the type each slot holds, over the whole fn (see gen_function) */
    int widened;        /* a slot's type changed in this pass: translate again */
    int failed;
} Gen;

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
    g->failed = 1;
    return 0;
}

static const char *bail(Gen *g) { return g->mode ? "return LUBE_FAIL;" : "return 1;"; }
static int new_tmp(Gen *g) { return g->tmp++; }
static char letter(int t) { return t == NT_NUM ? 'd' : t == NT_INT ? 'i' : 'b'; }
static const char *ctype(int t) { return t == NT_NUM ? "double" : t == NT_INT ? "int64_t" : "int"; }

/* e (of type t) as a double. */
static void as_double(int t, const char *e, char *out) { snprintf(out, 64, t == NT_INT ? "((double)%s)" : "%s", e); }

/* A value of type t goes into slot: the slot's type joins t (a whole number
 * in a slot that also holds other Numbers is a double); sets *how to the
 * value converted to the slot's type. 0 if they cannot share it. */
static int slot_store(Gen *g, int slot, int t, const char *v, char *how) {
    int st = g->slots[slot];
    if (!st) st = g->slots[slot] = t;
    if (st == NT_INT && t == NT_NUM) {
        st = g->slots[slot] = NT_NUM;
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

static int gen_expr(Gen *g, Node *n, char *res);
static void gen_stmt(Gen *g, Node *n);

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
    VarRef *v = &callee->as.var;
    Cand *c = v->kind == VAR_SELF ? g->cur : v->kind == VAR_GLOBAL ? cand_for(g, v->name) : NULL;
    const char *fn = !c && v->kind == VAR_GLOBAL && !map_get(g->declared, OBJ_VAL(v->name), NULL)
                         ? builtin(v->name, args->count) : NULL;
    if (!c && !fn) return fail(g);
    int whole = 1;  /* every Number argument is whole: the callee's whole-number version can take them */
    for (int i = 0; i < args->count; i++) {
        ta[i] = gen_expr(g, args->items[i], a[i]);
        int want = !c ? NT_NUM : i < c->d->nparams ? c->params[i] : -1;
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
        int t = n->as.var.kind == VAR_LOCAL ? g->slots[n->as.var.index] : 0;
        if (!t) return fail(g);
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
    default:  /* lists, text, objects, closures...: not native */
        return fail(g);
    }
}

/* ---------- statements ---------- */

static void gen_assign(Gen *g, Node *n) {
    Node *target = n->as.assign.target;
    char v[64], r[64], cur[64];
    int t;
    if (target->kind != N_VAR || target->as.var.kind != VAR_LOCAL || !(t = gen_expr(g, n->as.assign.value, v))) {
        fail(g);
        return;
    }
    int slot = target->as.var.index, st = g->slots[slot];
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

static void gen_block(Gen *g, NodeList *stmts) {
    for (int i = 0; i < stmts->count && !g->failed; i++) gen_stmt(g, stmts->items[i]);
}

static void gen_stmt(Gen *g, Node *n) {
    char v[64], e[64], x[64], y[64];
    switch (n->kind) {
    case N_EXPR:
        if (n->as.expr->kind == N_ASSIGN) gen_assign(g, n->as.expr);
        else if (n->as.expr->kind == N_CALL) gen_call(g, n->as.expr, v, 0);
        else gen_expr(g, n->as.expr, v);
        break;
    case N_LET: {
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
        if (want == NT_NONE) sb_puts(g->out, "return 0;\n");
        else if (g->mode) sb_printf(g->out, "return %s;\n", v);
        else if (as_double(t, v, x), 1) sb_printf(g->out, "*out = %s;\nreturn 0;\n", x);
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
    if (g->mode) sb_printf(out, "static int64_t ni_%d(", k);
    else sb_printf(out, "static int nf_%d(", k);
    for (int i = 0; i < d->nparams; i++) sb_printf(out, "%s p%d, ", g->mode ? "int64_t" : "double", i);
    sb_printf(out, "%sint left) {\nif (left <= 0) %s\n", g->mode ? "" : "double *out, ", bail(g));
    for (int i = 0; i < nslots; i++)
        sb_printf(out, "double d%d = 0; int64_t i%d = 0; int b%d = 0; (void)d%d; (void)i%d; (void)b%d;\n", i, i, i, i, i, i);
    for (int i = 0; i < d->nparams; i++) {
        char p[64];
        snprintf(p, sizeof p, c->params[i] == NT_BOOL ? "(p%d != 0)" : "p%d", i);
        store(g, i, c->params[i] == NT_NUM && g->mode ? NT_INT : c->params[i], p);
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
    sb_printf(out, "%s\n}\n\n", c->ret == NT_NONE ? "return 0;" : bail(g));
}

/* Translates candidate c (in mode) into out, translating again while the
 * slots' types widen; 0 if it cannot be native this way. */
static int gen_function(Gen *g, Cand *c, int mode, StrBuf *out) {
    int nslots = c->d->nslots > 0 ? c->d->nslots : 1;
    g->cur = c;
    g->mode = mode;
    g->failed = 0;
    g->slots = xmalloc(sizeof(int) * (size_t)nslots);
    memset(g->slots, 0, sizeof(int) * (size_t)nslots);
    StrBuf pass;
    do {
        sb_init(&pass);
        gen_pass(g, c, &pass);
        if (!g->widened || g->failed) break;
        sb_free(&pass);
    } while (1);
    if (!g->failed) sb_putn(out, pass.data, pass.len);
    sb_free(&pass);
    free(g->slots);
    return !g->failed;
}

/* ---------- the program ---------- */

/* A fn the program declares that can be a candidate: Number and Bool in and out. */
static void consider(Gen *g, Node *s, uint64_t hash) {
    if (s->kind != N_LET || !s->as.let.is_fn) return;
    FuncDecl *d = s->as.let.value->as.func;
    if (d->nparams > NATIVE_MAX_PARAMS) return;
    Cand c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < d->nparams; i++) {
        c.params[i] = d->types ? ann_type(&d->types[i]) : NT_NONE;
        if (c.params[i] <= NT_NONE || d->modes[i] == PARAM_REF || d->defaults[i]) return;
    }
    c.ret = ann_type(&d->ret);
    if (c.ret < 0) return;
    c.d = d;
    c.key = s->as.let.var.name;
    c.hash = hash;
    c.ok = c.ok_int = 1;
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

static const char HEADER[] =
    "/* Generated by lube --build --native: the program's functions that compute only with Numbers\n"
    " * and Bools, in C. Each returns 0 with its result in *out, or 1 to let the VM run the call. */\n"
    "#include \"lube.h\"\n#include <math.h>\n\n"
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
    "static double lube_nlog2(double x, double base) { return log(x) / log(base); }\n\n";

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
                    if (!mode) k->ok = 0;
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
    sb_puts(&c, HEADER);
    int count = 0;
    for (int i = 0; i < g.ncands; i++) {
        Cand *k = &g.cands[i];
        if (!k->ok) continue;
        sb_printf(&c, "static int nf_%d(", i);
        for (int p = 0; p < k->d->nparams; p++) sb_puts(&c, "double, ");
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
        sb_printf(&names, "%s%s", count++ ? ", " : "", lube_shown(k->key));
    }
    sb_puts(&c, "const LubeNative lube_natives[] = {\n");
    for (int i = 0; i < g.ncands; i++) {
        Cand *k = &g.cands[i];
        if (!k->ok) continue;
        sb_printf(&c, "    {\"%s\", \"", k->key->chars);  /* (names are plain identifiers, :: in a package) */
        for (int p = 0; p < k->d->nparams; p++) sb_putc(&c, k->params[p] == NT_NUM ? 'N' : 'B');
        sb_printf(&c, ":%s\", 0x%llxull, w_%d},\n", k->ret == NT_NUM ? "N" : k->ret == NT_BOOL ? "B" : "",
                  (unsigned long long)k->hash, i);
    }
    sb_printf(&c, "    {0, 0, 0, 0}\n};\nconst int lube_nnatives = %d;\n", count);

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
    return status;
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
