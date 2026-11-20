/*
 * plugin.c - plugins: packages written in C (see lube_plugin.h), loaded
 * when a program imports them.
 *
 *   import net                   loads the plugin net (once per process):
 *   net.tcp_listen(7777)         its functions and types are the package's
 *                                names, as a Lube package's would be.
 *
 * A package folder of that name comes first; then a plugin compiled into
 * this lube (lube_static_plugins: a program built with --native or --small
 * has the plugins it uses inside); then a shared library, net.so, net.dylib
 * or net.dll, in the folders of LUBE_PLUGINS, in plugins/ next to lube (or
 * next to a built program), or in ../lib/lube/plugins/ from lube. Its entry
 * point lube_plugin_init_net declares what it has through the LubeAPI table.
 *
 * Each interpreter (each isolate) gets natives of its own for the plugin's
 * functions, and a class for each of its types: objects of a plugin's type
 * are ObjForeign, whose class says which plugin type they are.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* dlopen, readlink, realpath under -std=c99 */
#endif
#include "lube.h"
#include "lube_plugin.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define TokenType WinTokenType  /* winnt.h has a TokenType of its own */
#include <windows.h>
#undef TokenType
#define PLUGIN_EXT ".dll"
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#define PLUGIN_EXT ".dylib"
extern int _NSGetExecutablePath(char *buf, uint32_t *size);  /* (mach-o/dyld.h) */
#else
#define PLUGIN_EXT ".so"
#endif
#endif

const char *lube_argv0;

/* ======================================================================
 * What plugins declare (once per process, kept until it ends)
 * ==================================================================== */

typedef struct {
    const char *name;
    LubeFn fn;
    int min, max;
    void *data;              /* (a host program's function: what it gave with it) */
} Fn;

struct LubeType {
    int id;                  /* where each interpreter keeps its class (foreign_classes) */
    char *name;
    struct LubePlugin *plugin;
    size_t size;
    void (*release)(void *data);
    int (*describe)(const void *data, char *buf, size_t size);
    Fn ctor;                 /* (fn NULL: calling the type makes nothing) */
    Fn *methods;
    int nmethods, cap;
};

struct LubePlugin {
    char *name;
    char *file;              /* where it was loaded from (NULL: compiled in) */
    Fn *fns;
    int nfns, cap;
    LubeType **types;
    int ntypes, tcap;
    int ok;                  /* 0: it would not load (said once) */
};

static LubeLock *registry;   /* over everything here: isolates import too */
static LubePlugin **plugins;
static int nplugins, plugins_cap, ntypes;
static LubeStaticPlugin *added;  /* plugins a host program compiled into itself (lube_plugin_add) */
static int nadded;

void lube_plugins_start(void) {
    if (!registry) registry = lock_new();
}

static Value V(LubeValue x) {
    Value v;
    memcpy(&v, &x, sizeof v);
    return v;
}

static LubeValue L(Value v) {
    LubeValue x;
    memcpy(&x, &v, sizeof x);
    return x;
}

typedef char value_sizes_match[sizeof(Value) == sizeof(LubeValue) ? 1 : -1];  /* (LubeValue is a Value) */

/* Every plugin function is called through this native: it finds which in I->calling. */
static Value plugin_call(Interp *I, int argc, Value *argv) {
    const Fn *f = (const Fn *)I->calling->plugin_fn;
    LubeValue small[8] = {{{0}}};  /* (zeroed: gcc would rather it were, with no arguments) */
    LubeValue *args = argc <= 8 ? small : (LubeValue *)xmalloc(sizeof(LubeValue) * (size_t)argc);
    if (argc) memcpy(args, argv, sizeof(LubeValue) * (size_t)argc);
    LubeValue r = f->fn((LubeVM *)I, argc, args);
    if (args != small) free(args);
    Value v = V(r);
    if (I->exiting && !IS_ERR(v)) {  /* (a host program's function called Lube, which called exit(): on out) */
        release(v);
        v = lube_error(I, "exit(%d)", I->exit_code);
    }
    return v;
}

static Value native_of(const Fn *f) {
    ObjNative *n = new_native(f->name, plugin_call, f->min, f->max, 0);
    n->plugin_fn = f;
    return OBJ_VAL(n);
}

static void add_fn(Fn **fns, int *n, int *cap, const char *name, LubeFn fn, int min, int max) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *fns = (Fn *)xrealloc(*fns, sizeof(Fn) * (size_t)*cap);
    }
    Fn *f = &(*fns)[(*n)++];
    f->name = xstrndup(name, strlen(name));
    f->fn = fn;
    f->min = min;
    f->max = max;
    f->data = NULL;
}

/* ======================================================================
 * The API (lube_plugin.h)
 * ==================================================================== */

static void api_function(LubePlugin *p, const char *name, LubeFn fn, int min, int max) {
    add_fn(&p->fns, &p->nfns, &p->cap, name, fn, min, max);
}

static LubeType *api_type(LubePlugin *p, const char *name, size_t size, void (*release_fn)(void *),
                          int (*describe)(const void *, char *, size_t)) {
    LubeType *t = (LubeType *)xmalloc(sizeof(LubeType));
    memset(t, 0, sizeof *t);
    t->id = ntypes++;  /* (under the registry lock: plugins load one at a time) */
    t->name = xstrndup(name, strlen(name));
    t->plugin = p;
    t->size = size;
    t->release = release_fn;
    t->describe = describe;
    if (p->ntypes == p->tcap) {
        p->tcap = p->tcap ? p->tcap * 2 : 4;
        p->types = (LubeType **)xrealloc(p->types, sizeof(LubeType *) * (size_t)p->tcap);
    }
    p->types[p->ntypes++] = t;
    return t;
}

static void api_method(LubeType *t, const char *name, LubeFn fn, int min, int max) {
    add_fn(&t->methods, &t->nmethods, &t->cap, name, fn, min, max);
}

static void api_constructor(LubeType *t, LubeFn fn, int min, int max) {
    t->ctor.name = t->name;
    t->ctor.fn = fn;
    t->ctor.min = min;
    t->ctor.max = max;
}

static int api_kind(LubeValue x) {
    Value v = V(x);
    if (IS_NIL(v)) return LUBE_NOTHING;
    if (IS_BOOL(v)) return LUBE_BOOL;
    if (IS_NUMERIC(v)) return LUBE_NUMBER;
    if (IS_TEXT(v)) return LUBE_STR;
    if (IS_BYTES(v)) return LUBE_BYTES;
    if (IS_LIST(v)) return LUBE_LIST;
    if (IS_MAP(v)) return LUBE_OBJECT;
    if (IS_FOREIGN(v)) return LUBE_FOREIGN;
    return LUBE_OTHER;
}

static double api_number(LubeValue x) {
    Value v = V(x);
    return IS_NUMERIC(v) ? numeric_value(v) : 0;
}

static int api_truthy(LubeValue x) { return is_truthy(V(x)); }

static const char *api_str(LubeValue x, size_t *len) {
    Value v = V(x);
    if (!IS_TEXT(v)) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = text_of(v)->length;
    return text_of(v)->chars;
}

static const unsigned char *api_bytes(LubeValue x, size_t *len) {
    Value v = V(x);
    if (!IS_BYTES(v)) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = AS_BYTES(v)->len;
    return AS_BYTES(v)->data ? AS_BYTES(v)->data : (const unsigned char *)"";
}

static size_t api_list_len(LubeValue x) {
    Value v = V(x);
    return IS_LIST(v) ? AS_LIST(v)->count : 0;
}

static LubeValue api_list_get(LubeValue x, size_t i) {
    Value v = V(x);
    return L(IS_LIST(v) && i < AS_LIST(v)->count ? AS_LIST(v)->items[i] : NIL_VAL);
}

static int api_object_get(LubeVM *vm, LubeValue o, const char *key, LubeValue *out) {
    Value v = V(o), got;
    if (!IS_MAP(v) || !map_get(AS_MAP(v), OBJ_VAL(lube_intern((Interp *)vm, key, strlen(key))), &got)) return 0;
    *out = L(got);
    return 1;
}

static const char *api_type_name(LubeValue x) { return type_name(V(x)); }
static const char *api_a_type(LubeValue x) { return a_type(V(x)); }
static LubeValue api_nothing(void) { return L(NIL_VAL); }
static LubeValue api_boolean(int b) { return L(BOOL_VAL(b != 0)); }
static LubeValue api_number_value(double x) { return L(NUM_VAL(x)); }

static LubeValue api_new_str(LubeVM *vm, const char *s, size_t len) {
    (void)vm;
    if (len > LUBE_MAX_BYTES) lube_out_of_memory();
    return L(OBJ_VAL(new_string(s, len)));
}

static LubeValue api_new_bytes(LubeVM *vm, const void *data, size_t len) {
    (void)vm;
    if (len > LUBE_MAX_BYTES) lube_out_of_memory();
    ObjBytes *b = new_bytes(len);
    if (len) {
        if (data) memcpy(b->data, data, len);
        else memset(b->data, 0, len);
    }
    b->len = len;
    return L(OBJ_VAL(b));
}

static unsigned char *api_bytes_buffer(LubeValue x) {
    Value v = V(x);
    return IS_BYTES(v) ? AS_BYTES(v)->data : NULL;
}

static void api_bytes_resize(LubeValue x, size_t len) {
    Value v = V(x);
    if (!IS_BYTES(v)) return;
    ObjBytes *b = AS_BYTES(v);
    if (len > b->len) {
        bytes_reserve(b, len);
        memset(b->data + b->len, 0, len - b->len);
    }
    b->len = len;
}

static LubeValue api_new_list(LubeVM *vm) {
    (void)vm;
    return L(OBJ_VAL(new_list()));
}

static void api_list_push(LubeValue list, LubeValue item) {
    Value l = V(list);
    if (IS_LIST(l) && AS_LIST(l)->elem == TT_ANY) list_push(AS_LIST(l), V(item));
    else release(V(item));
}

static LubeValue api_new_object(LubeVM *vm) {
    (void)vm;
    return L(OBJ_VAL(new_map()));
}

static void api_object_set(LubeVM *vm, LubeValue o, const char *key, LubeValue x) {
    Value v = V(o);
    if (IS_MAP(v) && !AS_MAP(v)->klass) map_set(AS_MAP(v), retain(OBJ_VAL(lube_intern((Interp *)vm, key, strlen(key)))), V(x));
    else release(V(x));
}

/* The class of plugin type t in I (made the first time). */
static ObjClass *foreign_class(Interp *I, LubeType *t) {
    if (t->id < I->nforeign && I->foreign_classes[t->id]) return I->foreign_classes[t->id];
    if (t->id >= I->nforeign) {
        int n = t->id + 8;
        I->foreign_classes = (ObjClass **)xrealloc(I->foreign_classes, sizeof(ObjClass *) * (size_t)n);
        memset(I->foreign_classes + I->nforeign, 0, sizeof(ObjClass *) * (size_t)(n - I->nforeign));
        I->nforeign = n;
    }
    ObjClass *c = (ObjClass *)alloc_object(sizeof(ObjClass), OBJ_CLASS);
    c->name = lube_intern(I, t->name, strlen(t->name));
    c->key = lube_global_key(I, lube_intern(I, t->plugin->name, strlen(t->plugin->name)), c->name);
    c->parent = NULL;
    c->nfields = 0;
    c->field_names = NULL;
    c->field_defaults = NULL;
    c->field_types = NULL;
    c->methods = new_map();
    for (int i = 0; i < t->nmethods; i++)
        map_set(c->methods, retain(OBJ_VAL(lube_intern(I, t->methods[i].name, strlen(t->methods[i].name)))),
                native_of(&t->methods[i]));
    c->constructor = t->ctor.fn ? native_of(&t->ctor) : NIL_VAL;
    c->foreign = t;
    list_push(I->classes, OBJ_VAL(c));  /* (kept until the end, as every class) */
    I->foreign_classes[t->id] = c;
    return c;
}

static LubeValue api_new_foreign(LubeVM *vm, const LubeType *type, void **data) {
    LubeType *t = (LubeType *)type;
    ObjClass *c = foreign_class((Interp *)vm, t);
    ObjForeign *f = (ObjForeign *)alloc_object(sizeof(ObjForeign) + t->size, OBJ_FOREIGN);
    f->klass = c;
    memset(f->data, 0, t->size);
    if (data) *data = f->data;
    return L(OBJ_VAL(f));
}

static void *api_foreign(LubeValue x, const LubeType *t) {
    Value v = V(x);
    return IS_FOREIGN(v) && AS_FOREIGN(v)->klass->foreign == t ? (void *)AS_FOREIGN(v)->data : NULL;
}

static LubeValue api_retain(LubeValue x) { return L(retain(V(x))); }
static void api_release(LubeValue x) { release(V(x)); }

static LubeValue api_error(LubeVM *vm, const char *format, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    return L(lube_error((Interp *)vm, "%s", text));
}

static double api_now(void) { return lube_now(); }

static void *api_function_data(LubeVM *vm) {
    const ObjNative *n = ((Interp *)vm)->calling;
    return n && n->plugin_fn ? ((const Fn *)n->plugin_fn)->data : NULL;
}
static void api_format_number(double x, char *buf, size_t size) { format_number(buf, size, x); }

static const LubeAPI API = {
    LUBE_PLUGIN_API, sizeof(LubeAPI),
    api_function, api_type, api_method, api_constructor,
    api_kind, api_number, api_truthy, api_str, api_bytes, api_list_len, api_list_get, api_object_get,
    api_type_name, api_a_type,
    api_nothing, api_boolean, api_number_value, api_new_str, api_new_bytes, api_bytes_buffer, api_bytes_resize,
    api_new_list, api_list_push, api_new_object, api_object_set,
    api_new_foreign, api_foreign,
    api_retain, api_release, api_error,
    api_now, api_format_number, lock_new, lock_acquire, lock_release, lock_wait, lock_notify, thread_start,
    api_function_data,
};

const LubeAPI *lube_plugin_api(void) { return &API; }

/* A host program's function (lube_embed.h), as a native; lube_free_host_fn lets go of it. */
Value lube_host_native(Interp *I, const char *name, LubeFn fn, int min, int max, void *data) {
    (void)I;
    Fn *f = (Fn *)xmalloc(sizeof(Fn));
    f->name = xstrndup(name, strlen(name));
    f->fn = fn;
    f->min = min;
    f->max = max;
    f->data = data;
    return native_of(f);
}

void lube_free_host_fn(const void *fn) {
    free((void *)((const Fn *)fn)->name);
    free((void *)fn);
}

/* ======================================================================
 * Finding and loading plugins
 * ==================================================================== */

static int name_ok(const char *s) {  /* (a plain name: never a path) */
    if (!*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

static int is_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* The folder lube itself is in (malloc'd), or NULL. */
static char *own_folder(void) {
    char *path = NULL;
#ifdef _WIN32
    path = os_self_path();
#else
    char buf[PATH_MAX + 1], real[PATH_MAX + 1];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0 && realpath(buf, real)) path = xstrndup(real, strlen(real));
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        path = xstrndup(buf, (size_t)n);
    }
#endif
    if (!path && lube_argv0 && strchr(lube_argv0, '/') && realpath(lube_argv0, real)) path = xstrndup(real, strlen(real));  /* LCOV_EXCL_LINE */
#endif
    if (!path) return NULL;  /* LCOV_EXCL_LINE: the system would not say */
    lube_slashes(path);
    char *slash = strrchr(path, '/');
    if (slash) *slash = '\0';
    else path[0] = '\0';  /* LCOV_EXCL_LINE */
    return path;
}

/* Where plugin `name`'s library is (malloc'd), or NULL. */
static char *find_file(const char *name) {
    StrBuf path;
    sb_init(&path);
    const char *dirs = getenv("LUBE_PLUGINS");
    while (dirs && *dirs) {
        const char *end = strchr(dirs, LUBE_PATH_SEP);
        size_t len = end ? (size_t)(end - dirs) : strlen(dirs);
        path.len = 0;
        sb_putn(&path, dirs, len);
        sb_printf(&path, "/%s%s", name, PLUGIN_EXT);
        lube_slashes(path.data);
        if (len && is_file(path.data)) return path.data;
        dirs = end ? end + 1 : NULL;
    }
    char *home = own_folder();
    static const char *const under[] = {"/plugins/", "/../lib/lube/plugins/"};
    for (size_t i = 0; home && i < sizeof under / sizeof under[0]; i++) {
        path.len = 0;
        sb_printf(&path, "%s%s%s%s", home, under[i], name, PLUGIN_EXT);
        if (is_file(path.data)) {
            free(home);
            return path.data;
        }
    }
    free(home);
    sb_free(&path);
    return NULL;
}

typedef int (*InitFn)(LubePlugin *, const LubeAPI *);

/* The library's entry point, or NULL after saying why. */
static InitFn open_library(const char *name, const char *file) {
    char symbol[128];
    snprintf(symbol, sizeof symbol, "lube_plugin_init_%s", name);
    InitFn init = NULL;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(file);
    if (!lib) {
        lube_print(1, "lube: cannot load plugin '%s' from '%s': Windows error %lu\n", name, file, (unsigned long)GetLastError());
        return NULL;
    }
    init = (InitFn)(void (*)(void))GetProcAddress(lib, symbol);
#else
    void *lib = dlopen(file, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        lube_print(1, "lube: cannot load plugin '%s' from '%s': %s\n", name, file, dlerror());
        return NULL;
    }
    void *sym = dlsym(lib, symbol);
    memcpy(&init, &sym, sizeof init);  /* (no ISO C cast from data to code, and no type punning) */
#endif
    if (!init) lube_print(1, "lube: '%s' is not a Lube plugin (it has no %s)\n", file, symbol);
    return init;
}

/* Plugin `name`, loaded the first time (under the registry lock): NULL if
 * there is none; one with ok 0 if it would not load (said then). */
static LubePlugin *load(const char *name) {
    for (int i = 0; i < nplugins; i++)
        if (!strcmp(plugins[i]->name, name)) return plugins[i];
    InitFn init = NULL;
    char *file = NULL;
    for (int i = 0; i < nadded && !init; i++)
        if (!strcmp(added[i].name, name)) init = added[i].init;
    for (const LubeStaticPlugin *s = lube_static_plugins; s->name && !init; s++)
        if (!strcmp(s->name, name)) init = s->init;
    if (!init) {
        file = find_file(name);
        if (!file) return NULL;
        init = open_library(name, file);
    }
    LubePlugin *p = (LubePlugin *)xmalloc(sizeof(LubePlugin));
    memset(p, 0, sizeof *p);
    p->name = xstrndup(name, strlen(name));
    p->file = file;
    if (init) {
        int version = init(p, &API);
        if (version < 0) {
            lube_print(1, "lube: plugin '%s' could not start\n", name);
        } else if (version > LUBE_PLUGIN_API) {
            lube_print(1, "lube: plugin '%s' needs a newer Lube (it was made for plugin API %d; this Lube has %d)\n",
                       name, version, LUBE_PLUGIN_API);
        } else {
            p->ok = 1;
        }
    }
    if (nplugins == plugins_cap) {
        plugins_cap = plugins_cap ? plugins_cap * 2 : 4;
        plugins = (LubePlugin **)xrealloc(plugins, sizeof(LubePlugin *) * (size_t)plugins_cap);
    }
    plugins[nplugins++] = p;
    return p;
}

/* A plugin a host program compiled into itself: `import name` finds it first. */
void lube_plugin_add(const char *name, int (*init)(struct LubePlugin *plugin, const struct LubeAPI *api)) {
    lube_plugins_start();
    lock_acquire(registry);
    added = (LubeStaticPlugin *)xrealloc(added, sizeof(LubeStaticPlugin) * (size_t)(nadded + 1));
    added[nadded].name = xstrndup(name, strlen(name));
    added[nadded++].init = init;
    lock_release(registry);
}

static LubePlugin *get(ObjString *name) {
    if (!name_ok(name->chars)) return NULL;
    lock_acquire(registry);
    LubePlugin *p = load(name->chars);
    lock_release(registry);
    return p;
}

/* The names plugin `name` has (a new map), or NULL; *bad: it would not load (2). */
ObjMap *lube_plugin_names(Interp *I, ObjString *name, int *bad) {
    LubePlugin *p = get(name);
    if (!p) return NULL;
    if (!p->ok) {
        *bad = 2;
        return NULL;
    }
    ObjMap *names = new_map();
    for (int i = 0; i < p->nfns; i++)
        map_set(names, retain(OBJ_VAL(lube_intern(I, p->fns[i].name, strlen(p->fns[i].name)))), BOOL_VAL(1));
    for (int i = 0; i < p->ntypes; i++)
        map_set(names, retain(OBJ_VAL(lube_intern(I, p->types[i]->name, strlen(p->types[i]->name)))), BOOL_VAL(1));
    return names;
}

/* import name: the plugin's functions and types, as the package's globals in I. */
int lube_plugin_import(Interp *I, ObjString *name) {
    LubePlugin *p = get(name);
    if (!p || !p->ok) {  /* LCOV_EXCL_START: found and loaded when the import was compiled */
        lube_error(I, "cannot import %s: there is no such package or plugin", name->chars);
        return -1;
    }  /* LCOV_EXCL_STOP */
    for (int i = 0; i < p->nfns; i++) {
        ObjString *key = lube_global_key(I, name, lube_intern(I, p->fns[i].name, strlen(p->fns[i].name)));
        if (lube_define_global(I, key, native_of(&p->fns[i])) < 0) return -1;  /* LCOV_EXCL_LINE */
    }
    for (int i = 0; i < p->ntypes; i++) {
        ObjClass *c = foreign_class(I, p->types[i]);
        if (lube_define_global(I, c->key, retain(OBJ_VAL(c))) < 0) return -1;  /* LCOV_EXCL_LINE */
    }
    return 0;
}

/* The library plugin `name` came from (NULL: compiled in, or not loaded). */
const char *lube_plugin_file(Interp *I, ObjString *name) {
    (void)I;
    LubePlugin *p = get(name);
    return p && p->ok ? p->file : NULL;
}

#ifndef LUBE_RUNTIME  /* (lube --build's) */
/* lube --build: plugin `name`'s library, copied into plugins/ next to the
 * built program `exe` (which finds it there). 1 if there is nothing to copy
 * (it is inside this lube, and so inside the program), 0 when copied (with
 * where, as said to a person, in *copied), -1 after saying why not. */
int lube_plugin_copy(Interp *I, ObjString *name, const char *exe, StrBuf *copied) {
    const char *file = lube_plugin_file(I, name);
    if (!file) return 1;
    size_t len;
    char *data = read_file(file, &len);
    StrBuf dir;
    sb_init(&dir);
    const char *slash = strrchr(exe, '/');
    if (slash) sb_putn(&dir, exe, (size_t)(slash - exe + 1));
    sb_puts(&dir, "plugins");
#ifdef _WIN32
    _mkdir(dir.data);
#else
    mkdir(dir.data, 0777);
#endif
    copied->len = 0;
    sb_printf(copied, "plugins/%s%s", name->chars, PLUGIN_EXT);  /* (as said: from the program's folder) */
    sb_printf(&dir, "/%s%s", name->chars, PLUGIN_EXT);
    FILE *out = data ? fopen(dir.data, "wb") : NULL;
    int ok = out && fwrite(data, 1, len, out) == len;
    if (out && fclose(out)) ok = 0;  /* LCOV_EXCL_LINE: a full disk */
#ifndef _WIN32
    if (ok) chmod(dir.data, 0755);
#endif
    if (!ok) fprintf(stderr, "lube: cannot copy plugin %s to '%s'\n", name->chars, dir.data);
    free(data);
    sb_free(&dir);
    return ok ? 0 : -1;
}
#endif

/* Calling a plugin's type: its constructor, as Sound("jump.wav"). */
Value lube_foreign_construct(Interp *I, ObjClass *c, int argc, Value *argv) {
    if (IS_NIL(c->constructor))
        return lube_error(I, "%s cannot be made by calling it (its package makes them)", c->name->chars);
    ObjNative *n = AS_NATIVE(c->constructor);
    if (argc < n->min_args || (n->max_args >= 0 && argc > n->max_args)) {
        if (n->min_args == n->max_args)
            return lube_error(I, "%s() takes %d argument%s but got %d", n->name, n->min_args, n->min_args == 1 ? "" : "s", argc);
        if (n->max_args < 0)
            return lube_error(I, "%s() takes at least %d argument%s but got %d", n->name, n->min_args, n->min_args == 1 ? "" : "s", argc);
        return lube_error(I, "%s() takes %d to %d arguments but got %d", n->name, n->min_args, n->max_args, argc);
    }
    return (I->calling = n)->fn(I, argc, argv);
}

void lube_foreign_free(ObjForeign *f) {
    LubeType *t = f->klass->foreign;
    if (t->release) t->release(f->data);
}

void lube_foreign_describe(StrBuf *sb, Value v) {
    ObjForeign *f = AS_FOREIGN(v);
    LubeType *t = f->klass->foreign;
    char small[256];
    int n = t->describe ? t->describe(f->data, small, sizeof small) : -1;
    if (n < 0) {
        sb_printf(sb, "<%s>", t->name);
    } else if ((size_t)n < sizeof small) {
        sb_putn(sb, small, (size_t)n);
    } else {  /* long: again, with room for it */
        char *big = (char *)xmalloc((size_t)n + 1);
        t->describe(f->data, big, (size_t)n + 1);
        sb_putn(sb, big, (size_t)n);
        free(big);
    }
}
