/*
 * vec.c - a plugin of one's own, for the tests (tests/cli.sh builds it): the
 * parts of the plugin API that version 2 added. A Vec holds up to 8 Numbers
 * and has every operator; it uses the plugin greet (lube->require).
 */
#include "../../src/lube_plugin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;
static LubeType *Vec, *Odd;
static const void *greet_gave, *self_gave, *none_gave, *path_gave;
static const char exports[] = "vec's table";

typedef struct {
    int n;
    double v[8];
} V;

static int describe(const void *data, char *buf, size_t size) {
    const V *x = (const V *)data;
    char text[200];
    size_t at = 0;
    for (int i = 0; i < x->n; i++) {
        char num[32];
        lube->format_number(x->v[i], num, sizeof num);
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%s", i ? ", " : "", num);
    }
    return snprintf(buf, size, "Vec(%s)", text);
}

static LubeValue make(LubeVM *vm, int n, V **out) {
    void *data;
    LubeValue r = lube->new_foreign(vm, Vec, &data);
    *out = (V *)data;
    (*out)->n = n;
    return r;
}

FN(vec_new) {
    V *x;
    for (int i = 0; i < argc; i++)
        if (lube->kind(argv[i]) != LUBE_NUMBER) return lube->error(vm, "Vec() takes Numbers");
    LubeValue r = make(vm, argc, &x);
    for (int i = 0; i < argc; i++) x->v[i] = lube->number(argv[i]);
    return r;
}

/* An operand: a Vec's numbers, or one Number for each. 0 if it is neither. */
static int operand(LubeValue a, int n, double *out) {
    V *x = (V *)lube->foreign(a, Vec);
    if (x) {
        if (x->n != n) return 0;
        memcpy(out, x->v, sizeof(double) * (size_t)n);
        return 1;
    }
    if (lube->kind(a) != LUBE_NUMBER) return 0;
    for (int i = 0; i < n; i++) out[i] = lube->number(a);
    return 1;
}

static int size_of(const LubeValue *argv) {
    V *x = (V *)lube->foreign(argv[0], Vec);
    return (x ? x : (V *)lube->foreign(argv[1], Vec))->n;
}

#define ARITH(name, expr)                                                                       \
    FN(name) {                                                                                  \
        (void)argc;                                                                             \
        int n = size_of(argv);                                                                  \
        double a[8], b[8];                                                                      \
        if (!operand(argv[0], n, a) || !operand(argv[1], n, b))                                 \
            return lube->error(vm, "a Vec works with a Vec of its size, or a Number");          \
        V *r;                                                                                   \
        LubeValue out = make(vm, n, &r);                                                        \
        for (int i = 0; i < n; i++) r->v[i] = (expr);                                           \
        return out;                                                                             \
    }
ARITH(op_add, a[i] + b[i])
ARITH(op_sub, a[i] - b[i])
ARITH(op_mul, a[i] * b[i])
ARITH(op_div, a[i] / b[i])
ARITH(op_mod, fmod(a[i], b[i]))
ARITH(op_pow, pow(a[i], b[i]))
ARITH(op_lt, a[i] < b[i])  /* (a Vec of 0s and 1s: not a Bool) */
ARITH(op_le, a[i] <= b[i])
ARITH(op_gt, a[i] > b[i])
ARITH(op_ge, a[i] >= b[i])

/* v @ w: the dot product */
FN(op_dot) {
    (void)argc;
    int n = size_of(argv);
    double a[8], b[8], sum = 0;
    if (!operand(argv[0], n, a) || !operand(argv[1], n, b)) return lube->error(vm, "@ needs two Vecs");
    for (int i = 0; i < n; i++) sum += a[i] * b[i];
    return lube->number_value(sum);
}

FN(op_eq) {
    (void)vm;
    (void)argc;
    int n = size_of(argv);
    double a[8], b[8];
    return lube->boolean(operand(argv[0], n, a) && operand(argv[1], n, b) && !memcmp(a, b, sizeof(double) * (size_t)n));
}

FN(op_ne) {
    LubeValue eq = op_eq(vm, argc, argv);
    return lube->boolean(!lube->truthy(eq));
}

FN(op_neg) {
    (void)argc;
    V *x = (V *)lube->foreign(argv[0], Vec), *r;
    LubeValue out = make(vm, x->n, &r);
    for (int i = 0; i < x->n; i++) r->v[i] = -x->v[i];
    return out;
}

/* An index: a whole Number in 0..n-1 (from the end when negative); -1 if not. */
static int index_of(LubeValue i, int n) {
    if (lube->kind(i) != LUBE_NUMBER) return -1;
    double d = lube->number(i);
    if (d < 0) d += n;
    return d >= 0 && d < n && d == floor(d) ? (int)d : -1;
}

/* v[i]; v[i, j] (a Vec of the two); v[a..b] (a Vec of those: an open end is the last) */
FN(op_get) {
    V *x = (V *)lube->foreign(argv[0], Vec), *r;
    double start, end, step;
    int inclusive;
    if (argc == 2 && lube->range(argv[1], &start, &end, &step, &inclusive)) {
        if (isinf(end)) end = x->n;
        int from = (int)start, to = (int)end + inclusive;
        if (from < 0 || to > x->n || from > to) return lube->error(vm, "Vec range out of range");
        LubeValue out = make(vm, to - from, &r);
        for (int i = from; i < to; i++) r->v[i - from] = x->v[i];
        return out;
    }
    int k[8];
    for (int i = 1; i < argc; i++)
        if ((k[i] = index_of(argv[i], x->n)) < 0) return lube->error(vm, "Vec index out of range");
    if (argc == 2) return lube->number_value(x->v[k[1]]);
    LubeValue out = make(vm, argc - 1, &r);
    for (int i = 1; i < argc; i++) r->v[i - 1] = x->v[k[i]];
    return out;
}

/* v[i] = n; v[i, j] = n sets both */
FN(op_set) {
    V *x = (V *)lube->foreign(argv[0], Vec);
    if (lube->kind(argv[argc - 1]) != LUBE_NUMBER) return lube->error(vm, "a Vec holds Numbers");
    for (int i = 1; i < argc - 1; i++) {
        int k = index_of(argv[i], x->n);
        if (k < 0) return lube->error(vm, "Vec index out of range");
        x->v[k] = lube->number(argv[argc - 1]);
    }
    return lube->nothing();
}

FN(op_len) {
    (void)vm;
    (void)argc;
    return lube->number_value(((V *)lube->foreign(argv[0], Vec))->n);
}

FN(op_copy) {
    (void)argc;
    V *x = (V *)lube->foreign(argv[0], Vec), *r;
    LubeValue out = make(vm, x->n, &r);
    memcpy(r->v, x->v, sizeof x->v);
    return out;
}

/* v += w: in place */
FN(op_iadd) {
    (void)argc;
    V *x = (V *)lube->foreign(argv[0], Vec);
    double b[8];
    if (!operand(argv[1], x->n, b)) return lube->error(vm, "a Vec works with a Vec of its size, or a Number");
    for (int i = 0; i < x->n; i++) x->v[i] += b[i];
    return lube->retain(argv[0]);
}

FN(prop_x) {
    (void)vm;
    (void)argc;
    return lube->number_value(((V *)lube->foreign(argv[0], Vec))->v[0]);
}

FN(prop_bad) {
    (void)argc;
    (void)argv;
    return lube->error(vm, "no such thing");
}

/* what lube->require gave: [greet's, its own, none of that name, a path] */
FN(required) {
    (void)argc;
    (void)argv;
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, lube->boolean(greet_gave != NULL));
    lube->list_push(r, lube->boolean(self_gave == exports));
    lube->list_push(r, lube->boolean(none_gave != NULL));
    lube->list_push(r, lube->boolean(path_gave != NULL));
    return r;
}

/* apply(f, args...): f(args...), through lube->call; [it, whether it failed] */
FN(apply) {
    LubeValue r = lube->call(vm, argv[0], argc - 1, argv + 1);
    if (lube->failed(r)) return r;
    LubeValue l = lube->new_list(vm);
    lube->list_push(l, r);
    lube->list_push(l, lube->boolean(lube->failed(r)));
    return l;
}

/* ask(obj, name, args...): obj.name(args...), through lube->call_method */
FN(ask) {
    const char *name = lube->str(argv[1], NULL);
    if (!name) return lube->error(vm, "ask() wants a method's name");
    return lube->call_method(vm, argv[0], name, argc - 2, argv + 2);
}

/* An Odd: its len is not a Number, and its items are errors */
FN(odd_new) {
    (void)argc;
    (void)argv;
    return lube->new_foreign(vm, Odd, NULL);
}

FN(odd_len) {
    (void)argc;
    (void)argv;
    return lube->new_str(vm, "many", 4);
}

FN(odd_get) {
    (void)argc;
    (void)argv;
    return lube->error(vm, "an Odd has no items");
}

FN(odd_set) {
    (void)argc;
    (void)argv;
    return lube->error(vm, "an Odd cannot change");
}

FN(odd_copy) {
    (void)argc;
    (void)argv;
    return lube->error(vm, "an Odd cannot be copied");
}

LUBE_PLUGIN(vec) {
    lube = api;
    if (api->version < 2) return -1;
    api->provide(plugin, exports);
    greet_gave = api->require(plugin, "greet");
    greet_gave = api->require(plugin, "greet");  /* (once is enough: it is known) */
    self_gave = api->require(plugin, "vec");
    none_gave = api->require(plugin, "nothing_of_the_kind");
    path_gave = api->require(plugin, "../greet");
    api->function(plugin, "required", required, 0, 0);
    api->function(plugin, "apply", apply, 1, -1);
    api->function(plugin, "ask", ask, 2, -1);
    api->constant(plugin, "golden", 1.618);
    api->constant(plugin, "dims", 8);
    Vec = api->type(plugin, "Vec", sizeof(V), NULL, describe);
    api->constructor(Vec, vec_new, 0, 8);
    api->op(Vec, "+", op_add);
    api->op(Vec, "-", op_sub);
    api->op(Vec, "*", op_mul);
    api->op(Vec, "/", op_div);
    api->op(Vec, "%", op_mod);
    api->op(Vec, "**", op_pow);
    api->op(Vec, "@", op_dot);
    api->op(Vec, "==", op_eq);
    api->op(Vec, "!=", op_ne);
    api->op(Vec, "<", op_lt);
    api->op(Vec, "<=", op_le);
    api->op(Vec, ">", op_gt);
    api->op(Vec, ">=", op_ge);
    api->op(Vec, "neg", op_neg);
    api->op(Vec, "[]", op_get);
    api->op(Vec, "[]=", op_set);
    api->op(Vec, "len", op_len);
    api->op(Vec, "copy", op_copy);
    api->op(Vec, "+=", op_iadd);
    api->op(Vec, "<=>", op_add);  /* (there is no such operator: said once, when it loads) */
    api->property(Vec, "x", prop_x);
    api->property(Vec, "bad", prop_bad);
    Odd = api->type(plugin, "Odd", 0, NULL, NULL);
    api->constructor(Odd, odd_new, 0, 0);
    api->op(Odd, "len", odd_len);
    api->op(Odd, "[]", odd_get);
    api->op(Odd, "[]=", odd_set);
    api->op(Odd, "copy", odd_copy);
    return LUBE_PLUGIN_API;
}
