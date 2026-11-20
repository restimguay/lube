/*
 * greet.c - a plugin of one's own, for the tests (tests/cli.sh builds it):
 * every part of the plugin API, from outside Lube's sources.
 */
#include "../../src/lube_plugin.h"

#include <stdio.h>
#include <string.h>

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;
static LubeType *Counter, *Plain, *Long, *Pair;
static int let_go;  /* how many Counters have been let go of */

typedef struct {
    double n;
    LubeValue tag;  /* a value it keeps (retained) */
} Count;

/* hello(name): "hello, name" */
FN(hello) {
    (void)argc;
    size_t n;
    const char *s = lube->str(argv[0], &n);
    if (!s) return lube->error(vm, "hello() wants a str, not %s", lube->a_type(argv[0]));
    char out[256];
    int len = snprintf(out, sizeof out, "hello, %.*s", (int)n, s);
    return lube->new_str(vm, out, (size_t)len);
}

/* kind(x): what the API says x is; kind(): what nothing is */
FN(kind) {
    static const char *const names[] = {"nothing", "bool", "number", "str", "bytes", "list", "object", "foreign", "other"};
    const char *k = names[lube->kind(argc ? argv[0] : lube->nothing())];
    return lube->new_str(vm, k, strlen(k));
}

/* look(x): [number(x), truthy(x), type_name(x), bytes length or -1, list length] */
FN(look) {
    (void)argc;
    LubeValue l = lube->new_list(vm);
    size_t n;
    const char *t = lube->type_name(argv[0]);
    lube->list_push(l, lube->number_value(lube->number(argv[0])));
    lube->list_push(l, lube->boolean(lube->truthy(argv[0])));
    lube->list_push(l, lube->new_str(vm, t, strlen(t)));
    lube->list_push(l, lube->number_value(lube->bytes(argv[0], &n) ? (double)n : -1));
    lube->list_push(l, lube->number_value((double)lube->list_len(argv[0])));
    lube->list_push(l, lube->retain(lube->list_get(argv[0], 0)));  /* (nothing, if it is not a list) */
    return l;
}

/* pick(object, key): object[key], or "none" */
FN(pick) {
    (void)argc;
    LubeValue v;
    if (lube->object_get(vm, argv[0], lube->str(argv[1], NULL), &v)) return lube->retain(v);
    return lube->new_str(vm, "none", 4);
}

/* made(): {name: "greet", sizes: Bytes(61 62 00), when: true} */
FN(made) {
    (void)argc;
    (void)argv;
    LubeValue o = lube->new_object(vm);
    lube->object_set(vm, o, "name", lube->new_str(vm, "greet", 5));
    LubeValue b = lube->new_bytes(vm, "ab", 2);
    lube->bytes_resize(b, 4);  /* (longer: zeros) */
    lube->bytes_resize(b, 3);
    lube->bytes_buffer(b)[2] = 0;
    lube->object_set(vm, o, "sizes", b);
    lube->object_set(vm, o, "when", lube->boolean(lube->now() > 0));
    lube->object_set(vm, o, "name", lube->new_str(vm, "greet!", 6));  /* (again: replaced) */
    return o;
}

/* misuse(list, object): what the API does with the wrong things (nothing) */
FN(misuse) {
    (void)argc;
    lube->list_push(argv[1], lube->number_value(1));   /* not a list */
    lube->object_set(vm, argv[0], "x", lube->number_value(1));  /* not an object */
    lube->bytes_resize(argv[0], 1);                     /* not bytes */
    LubeValue extra = lube->new_str(vm, "let go", 6);
    lube->release(extra);
    return lube->number_value(lube->bytes_buffer(argv[0]) == NULL);
}

/* fmt(x): x as Lube prints it */
FN(fmt) {
    (void)argc;
    char buf[64];
    lube->format_number(lube->number(argv[0]), buf, sizeof buf);
    return lube->new_str(vm, buf, strlen(buf));
}

FN(fail) {
    (void)argc;
    (void)argv;
    return lube->error(vm, "greet failed: %d", 42);
}

FN(released) {
    (void)argc;
    (void)argv;
    (void)vm;
    return lube->number_value(let_go);
}

FN(plain) {
    (void)argc;
    (void)argv;
    return lube->new_foreign(vm, Plain, NULL);
}

static void count_release(void *data) {
    Count *c = (Count *)data;
    lube->release(c->tag);
    let_go++;
}

static int count_describe(const void *data, char *buf, size_t size) {
    char n[64];
    lube->format_number(((const Count *)data)->n, n, sizeof n);
    return snprintf(buf, size, "Counter(%s)", n);
}

/* Counter(start, tag): a counter that keeps a tag */
FN(counter_new) {
    (void)argc;
    void *data;
    LubeValue c = lube->new_foreign(vm, Counter, &data);
    ((Count *)data)->n = lube->number(argv[0]);
    ((Count *)data)->tag = lube->retain(argv[1]);
    return c;
}

FN(counter_add) {
    (void)vm;
    (void)argc;
    Count *c = (Count *)lube->foreign(argv[0], Counter);
    c->n += lube->number(argv[1]);
    return lube->nothing();
}

FN(counter_tag) {
    (void)vm;
    (void)argc;
    return lube->retain(((Count *)lube->foreign(argv[0], Counter))->tag);
}

/* is_counter(x) */
FN(is_counter) {
    (void)vm;
    (void)argc;
    return lube->boolean(lube->foreign(argv[0], Counter) != NULL);
}

static int long_describe(const void *data, char *buf, size_t size) {
    (void)data;
    char text[400];
    memset(text, 'x', sizeof text - 1);
    text[sizeof text - 1] = '\0';
    return snprintf(buf, size, "Long(%s)", text);
}

FN(long_new) {
    (void)argc;
    (void)argv;
    return lube->new_foreign(vm, Long, NULL);
}

FN(pair_new) {
    (void)argc;
    (void)argv;
    return lube->new_foreign(vm, Pair, NULL);
}

LUBE_PLUGIN(greet) {
    lube = api;
    if (api->version < 1 || api->size < sizeof(LubeAPI)) return -1;
    api->function(plugin, "hello", hello, 1, 1);
    api->function(plugin, "kind", kind, 0, 1);
    api->function(plugin, "look", look, 1, 1);
    api->function(plugin, "pick", pick, 2, 2);
    api->function(plugin, "made", made, 0, 0);
    api->function(plugin, "misuse", misuse, 2, 2);
    api->function(plugin, "fmt", fmt, 1, 1);
    api->function(plugin, "fail", fail, 0, -1);
    api->function(plugin, "released", released, 0, 0);
    api->function(plugin, "plain", plain, 0, 0);
    api->function(plugin, "is_counter", is_counter, 1, 1);
    Counter = api->type(plugin, "Counter", sizeof(Count), count_release, count_describe);
    api->constructor(Counter, counter_new, 2, 2);
    api->method(Counter, "add", counter_add, 2, 2);
    api->method(Counter, "tag", counter_tag, 1, 1);
    Plain = api->type(plugin, "Plain", 0, NULL, NULL);
    Long = api->type(plugin, "Long", 0, NULL, long_describe);
    api->constructor(Long, long_new, 1, -1);
    Pair = api->type(plugin, "Pair", 0, NULL, NULL);
    api->constructor(Pair, pair_new, 1, 2);
    return LUBE_PLUGIN_API;
}
