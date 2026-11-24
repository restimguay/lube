/*
 * texts.c - a plugin that reads text every way the plugin API hands it one,
 * for the tests (tests/cli.sh builds it, tests/cli/plugin_texts.lube uses it).
 *
 * A str of a few bytes is held in its value, not in an object, yet the
 * pointers lube->str() gives must be good as the header promises: an
 * argument's until the function returns, an item's for as long as it is in
 * its list, a result's until it is released. Each function here keeps its
 * pointers for as long as it may, then reads through them.
 */
#include "../../src/lube_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;
static LubeValue kept_value;  /* keep()'s (retained) */
static int has_kept;

typedef struct {
    char *data;
    size_t n;
} Out;

static void out_add(Out *o, const char *s, size_t n) {
    o->data = (char *)realloc(o->data, o->n + n + 1);
    memcpy(o->data + o->n, s, n);
    o->n += n;
    o->data[o->n] = '\0';
}

static LubeValue out_str(LubeVM *vm, Out *o) {
    LubeValue v = lube->new_str(vm, o->data ? o->data : "", o->n);
    free(o->data);
    return v;
}

/* args(a, b, ...): every argument's text, read only after every pointer was taken: "a|b|..."; what is not text
 * is "?" */
FN(args) {
    const char *text[16];
    size_t len[16];
    Out o = {0};
    if (argc > 16) argc = 16;
    for (int i = 0; i < argc; i++) text[i] = lube->str(argv[i], &len[i]);
    for (int i = 0; i < argc; i++) {
        if (i) out_add(&o, "|", 1);
        if (text[i]) out_add(&o, text[i], len[i]);
        else out_add(&o, "?", 1);
        if (text[i] && (strlen(text[i]) > len[i] || text[i][len[i]] != '\0')) out_add(&o, "(no NUL after it)", 17);
    }
    return out_str(vm, &o);
}

/* items(list): every item's text, read only after every pointer was taken (hundreds of them) */
FN(items) {
    (void)argc;
    size_t n = lube->list_len(argv[0]);
    const char **text = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    size_t *len = (size_t *)malloc(sizeof(size_t) * (n ? n : 1));
    Out o = {0};
    for (size_t i = 0; i < n; i++) text[i] = lube->str(lube->list_get(argv[0], i), &len[i]);
    for (size_t i = 0; i < n; i++) {
        if (i) out_add(&o, ",", 1);
        if (text[i]) out_add(&o, text[i], len[i]);
        else out_add(&o, "?", 1);
    }
    free(text);
    free(len);
    return out_str(vm, &o);
}

/* fields(object): "key=value;..." for its keys in order, every pointer taken before any is read */
FN(fields) {
    (void)argc;
    LubeValue keys = lube->object_keys(vm, argv[0]);
    size_t n = lube->list_len(keys);
    const char **name = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    const char **text = (const char **)malloc(sizeof(char *) * (n ? n : 1));
    Out o = {0};
    for (size_t i = 0; i < n; i++) {
        LubeValue v = lube->nothing();
        name[i] = lube->str(lube->list_get(keys, i), NULL);
        lube->object_get(vm, argv[0], name[i], &v);
        text[i] = lube->str(v, NULL);
    }
    for (size_t i = 0; i < n; i++) {
        out_add(&o, name[i], strlen(name[i]));
        out_add(&o, "=", 1);
        out_add(&o, text[i] ? text[i] : "?", text[i] ? strlen(text[i]) : 1);
        out_add(&o, ";", 1);
    }
    free(name);
    free(text);
    lube->release(keys);
    return out_str(vm, &o);
}

/* calls(fn, x, times): fn(x)'s text, `times` times over, each result kept until all are had: "r r r" */
FN(calls) {
    (void)argc;
    int times = (int)lube->number(argv[2]);
    LubeValue *r = (LubeValue *)malloc(sizeof(LubeValue) * (size_t)(times ? times : 1));
    const char **text = (const char **)malloc(sizeof(char *) * (size_t)(times ? times : 1));
    Out o = {0};
    for (int i = 0; i < times; i++) {
        r[i] = lube->call(vm, argv[0], 1, &argv[1]);
        if (lube->failed(r[i])) {
            LubeValue err = r[i];
            for (int k = 0; k < i; k++) lube->release(r[k]);
            free(r);
            free(text);
            return err;
        }
        text[i] = lube->str(r[i], NULL);
    }
    for (int i = 0; i < times; i++) {
        if (i) out_add(&o, " ", 1);
        out_add(&o, text[i] ? text[i] : "?", text[i] ? strlen(text[i]) : 1);
    }
    for (int i = 0; i < times; i++) lube->release(r[i]);
    free(r);
    free(text);
    return out_str(vm, &o);
}

/* method(object, name): object.name()'s text */
FN(method) {
    (void)argc;
    LubeValue r = lube->call_method(vm, argv[0], lube->str(argv[1], NULL), 0, NULL);
    if (lube->failed(r)) return r;
    const char *s = lube->str(r, NULL);
    LubeValue copy = lube->new_str(vm, s ? s : "?", s ? strlen(s) : 1);
    lube->release(r);
    return copy;
}

/* keep(s): s is kept (retained) for kept(), which reads it in a later call */
FN(keep) {
    (void)argc;
    (void)vm;
    if (has_kept) lube->release(kept_value);
    kept_value = lube->retain(argv[0]);
    has_kept = 1;
    return lube->nothing();
}

/* kept(times): the kept text, read `times` times, each as it is had: "s s s" (or "changed", if one was not it) */
FN(kept) {
    (void)argc;
    int times = (int)lube->number(argv[0]);
    size_t n;
    const char *first = lube->str(kept_value, &n);
    char want[64];
    snprintf(want, sizeof want, "%s", first ? first : "?");
    Out o = {0};
    for (int i = 0; i < times; i++) {
        const char *s = lube->str(kept_value, NULL);
        if (!s || strcmp(s, want)) return lube->new_str(vm, "changed", 7);
        if (i < 3) {
            if (i) out_add(&o, " ", 1);
            out_add(&o, s, strlen(s));
        }
    }
    return out_str(vm, &o);
}

/* made(text): a str this makes itself, and reads back: [the str, its length as read back, whether it read the same] */
FN(made) {
    (void)argc;
    size_t n, back;
    const char *s = lube->str(argv[0], &n);
    LubeValue v = lube->new_str(vm, s, n);
    const char *again = lube->str(v, &back);
    LubeValue l = lube->new_list(vm);
    int same = again && back == n && !memcmp(again, s, n);
    lube->list_push(l, v);
    lube->list_push(l, lube->number_value((double)back));
    lube->list_push(l, lube->boolean(same));
    return l;
}

LUBE_PLUGIN(texts) {
    lube = api;
    api->function(plugin, "args", args, 0, -1);
    api->function(plugin, "items", items, 1, 1);
    api->function(plugin, "fields", fields, 1, 1);
    api->function(plugin, "calls", calls, 3, 3);
    api->function(plugin, "method", method, 2, 2);
    api->function(plugin, "keep", keep, 1, 1);
    api->function(plugin, "kept", kept, 1, 1);
    api->function(plugin, "made", made, 1, 1);
    return LUBE_PLUGIN_API;
}
