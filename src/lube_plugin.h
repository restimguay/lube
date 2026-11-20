/*
 * lube_plugin.h - writing a Lube plugin: a package in C.
 *
 * A plugin is a shared library (name.so, name.dylib or name.dll) with one
 * entry point, made by LUBE_PLUGIN(name). `import name` in a program loads
 * it, once per process, and its functions and types become the package's
 * names: name.function(...), name.Type, or bare with `import name.*`.
 *
 *   #include "lube_plugin.h"
 *   static const LubeAPI *lube;
 *
 *   static LubeValue twice(LubeVM *vm, int argc, const LubeValue *argv) {
 *       if (lube->kind(argv[0]) != LUBE_NUMBER) return lube->error(vm, "twice() needs a Number");
 *       return lube->number_value(2 * lube->number(argv[0]));
 *   }
 *
 *   LUBE_PLUGIN(greet) {                      // the plugin "greet": greet.so / .dylib / .dll
 *       lube = api;
 *       lube->function(plugin, "twice", twice, 1, 1);
 *       return LUBE_PLUGIN_API;
 *   }
 *
 *   cc -shared -fPIC -o greet.so greet.c          (Linux; macOS: -dynamiclib -o greet.dylib;
 *                                                  Windows: -shared -o greet.dll)
 *
 * Lube looks for it in the folders of LUBE_PLUGINS, then in plugins/ next
 * to lube (or next to a program built with lube --build), then in
 * ../lib/lube/plugins/ from lube. It needs nothing else: everything goes
 * through the LubeAPI table, so a plugin links against no part of Lube.
 *
 * Rules:
 *   - A function gets its arguments borrowed, and returns a new value (or
 *     lube->error(vm, ...), which raises a Lube error). Methods get their
 *     object as argv[0].
 *   - Values made by lube->new_*() are the plugin's until it returns them,
 *     passes them on (list_push, object_set: those take them over), or lets
 *     go of them with release(). To keep a borrowed value, retain() it.
 *   - Text and bytes pointers are good until the plugin returns.
 *   - lube_plugin_init runs once per process. A function can run in any
 *     isolate (thread): keep shared state under a lock (lube->lock_new()).
 *     A plugin's objects stay in the isolate that made them.
 */
#ifndef LUBE_PLUGIN_H
#define LUBE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#define LUBE_PLUGIN_API 2  /* this header's version; later ones only add to LubeAPI (2: operators, ranges,
                              plugins that use each other) */

#ifdef _WIN32
#define LUBE_PLUGIN_EXPORT __declspec(dllexport)
#else
#define LUBE_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct LubePlugin;
struct LubeAPI;

/* The entry point of plugin `name`. It declares the plugin's functions and
 * types, and returns LUBE_PLUGIN_API (or -1 when it cannot work here: the
 * import fails). */
#define LUBE_PLUGIN(name)                                                                              \
    LUBE_PLUGIN_EXPORT int lube_plugin_init_##name(struct LubePlugin *plugin, const struct LubeAPI *api); \
    LUBE_PLUGIN_EXPORT int lube_plugin_init_##name(struct LubePlugin *plugin, const struct LubeAPI *api)

typedef struct LubeVM LubeVM;            /* the interpreter running the call */
typedef struct LubePlugin LubePlugin;    /* a plugin being declared */
typedef struct LubeType LubeType;        /* a plugin's type, such as a Socket */
#ifndef LUBE_LOCK_TYPEDEF
#define LUBE_LOCK_TYPEDEF
typedef struct LubeLock LubeLock;        /* a mutex with a condition to wait on */
#endif
typedef struct { uint64_t opaque[2]; } LubeValue;  /* any Lube value: look at it through the API only */

typedef LubeValue (*LubeFn)(LubeVM *vm, int argc, const LubeValue *argv);

enum {  /* what lube->kind() says a value is */
    LUBE_NOTHING,  /* what a function returns when it has nothing to give (lube->nothing()) */
    LUBE_BOOL,
    LUBE_NUMBER,   /* a Number, or a sized integer (int32, uint64...) */
    LUBE_STR,      /* text: a str or a String */
    LUBE_BYTES,
    LUBE_LIST,
    LUBE_OBJECT,   /* an object, a Map, or an instance of a class */
    LUBE_FOREIGN,  /* an object of a plugin's type (see foreign()) */
    LUBE_OTHER     /* a function, a range, a channel... */
};

typedef struct LubeAPI {
    int version;   /* the Lube's LUBE_PLUGIN_API: at least the plugin's */
    size_t size;   /* sizeof(LubeAPI) as the Lube knows it */

    /* --- declaring, in lube_plugin_init --- */
    /* A function of the package; max_args -1 takes any number. */
    void (*function)(LubePlugin *plugin, const char *name, LubeFn fn, int min_args, int max_args);
    /* A type, whose objects carry `size` bytes of the plugin's data (zeroed at first). `release` (or
     * NULL) lets go of what the data holds when the last owner of the object does; `describe` (or
     * NULL) writes how it prints, such as "Socket(tcp 127.0.0.1:5000)", returning snprintf's count. */
    LubeType *(*type)(LubePlugin *plugin, const char *name, size_t size, void (*release)(void *data),
                      int (*describe)(const void *data, char *buf, size_t size));
    /* A method of a type: argv[0] is the object, and min_args and max_args count it. */
    void (*method)(LubeType *type, const char *name, LubeFn fn, int min_args, int max_args);
    /* What calling the type does, as in Sound("jump.wav"): it returns an object of the type. */
    void (*constructor)(LubeType *type, LubeFn fn, int min_args, int max_args);

    /* --- reading values --- */
    int (*kind)(LubeValue v);
    double (*number)(LubeValue v);                    /* a Number's value (0 if it is none) */
    int (*truthy)(LubeValue v);                       /* only false is false */
    const char *(*str)(LubeValue v, size_t *len);     /* text, NUL-terminated (NULL if not text) */
    const unsigned char *(*bytes)(LubeValue v, size_t *len);  /* Bytes' bytes (NULL if not Bytes) */
    size_t (*list_len)(LubeValue list);
    LubeValue (*list_get)(LubeValue list, size_t i);  /* borrowed */
    int (*object_get)(LubeVM *vm, LubeValue object, const char *key, LubeValue *out);  /* 1 if there (borrowed) */
    const char *(*type_name)(LubeValue v);            /* "Number", "Socket"... */
    const char *(*a_type)(LubeValue v);               /* "a Number", "a Socket"..., for messages */

    /* --- making values --- */
    LubeValue (*nothing)(void);
    LubeValue (*boolean)(int b);
    LubeValue (*number_value)(double x);
    LubeValue (*new_str)(LubeVM *vm, const char *s, size_t len);
    LubeValue (*new_bytes)(LubeVM *vm, const void *data, size_t len);  /* data NULL: len zero bytes */
    unsigned char *(*bytes_buffer)(LubeValue fresh);  /* where to write into Bytes just made by new_bytes */
    void (*bytes_resize)(LubeValue fresh, size_t len);  /* (likewise: a longer end is zeros) */
    LubeValue (*new_list)(LubeVM *vm);
    void (*list_push)(LubeValue list, LubeValue item);  /* takes item (a list just made) */
    LubeValue (*new_object)(LubeVM *vm);
    void (*object_set)(LubeVM *vm, LubeValue object, const char *key, LubeValue v);  /* takes v */

    /* --- a plugin's objects --- */
    LubeValue (*new_foreign)(LubeVM *vm, const LubeType *type, void **data);
    void *(*foreign)(LubeValue v, const LubeType *type);  /* its data if v is of that type, else NULL */

    /* --- owners and errors --- */
    LubeValue (*retain)(LubeValue v);
    void (*release)(LubeValue v);
    LubeValue (*error)(LubeVM *vm, const char *format, ...);  /* raises an error: return what it gives */

    /* --- the system, the same everywhere --- */
    double (*now)(void);                              /* a steady clock, in seconds */
    void (*format_number)(double x, char *buf, size_t size);  /* as Lube prints it */
    LubeLock *(*lock_new)(void);                      /* lives as long as the process */
    void (*lock_acquire)(LubeLock *lock);
    void (*lock_release)(LubeLock *lock);
    void (*lock_wait)(LubeLock *lock);                /* releases it while waiting for lock_notify */
    void (*lock_notify)(LubeLock *lock);              /* wakes every waiter */
    int (*thread_start)(void (*fn)(void *), void *arg, size_t stack_size);  /* 0 if it could not */

    /* --- the function being called --- */
    void *(*function_data)(LubeVM *vm);  /* the data a host program gave it (lube_embed.h), or NULL */

    /* --- version 2 (check api->version >= 2 before using these) --- */
    /* An operator of a type, in lube_plugin_init. Each is a LubeFn, and returns a new value or an error:
     *   "+" "-" "*" "/" "%" "**"  a + b...: argv is (a, b), and either may be the object (2 * x and x * 2)
     *   "==" "!=" "<" "<=" ">" ">="  likewise; they may return any value, not only a Bool
     *   "neg"   -x: argv is (x)
     *   "[]"    x[i] and x[i, j]: argv is (x, i) or (x, i, j); an index may be a range, and in brackets
     *           `..` alone is 0..inf, `2..` is 2..inf and `..3` is 0..3 (see range())
     *   "[]="   x[i] = v, x[i, j] = v: argv is (x, i..., v); it changes x and returns nothing
     *   "len"   len(x), and `for item in x`, which then takes x[0], x[1]... through "[]"
     *   "copy"  a copy that changes apart from x: x[i] = v copies first when x is shared (as a list
     *           would), so another variable holding x keeps what it had; without it, both see the change
     *   "+=" "-=" "*=" "/=" "%="  x += b, changing x in place when nothing else holds it; argv is (x, b),
     *           and it returns x (retained) or something new. Without them, x += b is x = x + b. */
    void (*op)(LubeType *type, const char *op, LubeFn fn);
    /* A range's numbers (1), or 0 if v is not a range: 0..5 is start 0, end 5, step 1, inclusive 0. */
    int (*range)(LubeValue v, double *start, double *end, double *step, int *inclusive);
    /* What this plugin gives other plugins (a table of its own functions, say), in lube_plugin_init. */
    void (*provide)(LubePlugin *plugin, const void *exports);
    /* Plugin `name`, loaded now if it is not yet: what it provided, or NULL if there is no such plugin or
     * it gave nothing. Only in lube_plugin_init (a program built with lube --build gets it too). */
    const void *(*require)(LubePlugin *plugin, const char *name);
    /* A Number of the package, as math.pi (in lube_plugin_init). */
    void (*constant)(LubePlugin *plugin, const char *name, double value);
    /* A property of a type: x.name calls fn with argv (x) and gives what it returns. It is read-only. */
    void (*property)(LubeType *type, const char *name, LubeFn fn);
    /* Calls a Lube function (or anything callable) with argv (borrowed): what it returns, or its error, which
     * the plugin returns as it is (failed() tells). */
    LubeValue (*call)(LubeVM *vm, LubeValue fn, int argc, const LubeValue *argv);
    /* Calls obj.name(argv...): a method of a class's instance, or of a plugin's object; likewise. */
    LubeValue (*call_method)(LubeVM *vm, LubeValue obj, const char *name, int argc, const LubeValue *argv);
    /* Whether v is an error (from call, call_method or error) rather than a value. */
    int (*failed)(LubeValue v);
    /* An object's (or Map's) keys, in order, as a new List; NULL's List (empty) for anything else. */
    LubeValue (*object_keys)(LubeVM *vm, LubeValue object);
} LubeAPI;

#ifdef __cplusplus
}
#endif

#endif
