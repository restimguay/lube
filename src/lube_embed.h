/*
 * lube_embed.h - Lube inside a C or C++ program: run Lube code, call its
 * functions, and give it functions (and whole plugins) of the program's.
 *
 *   #include "lube_embed.h"
 *
 *   static LubeValue add(LubeVM *vm, int argc, const LubeValue *argv) {
 *       const LubeAPI *lube = lube_api();
 *       return lube->number_value(lube->number(argv[0]) + lube->number(argv[1]));
 *   }
 *
 *   int main(void) {
 *       LubeVM *vm = lube_open();
 *       lube_function(vm, "add", add, 2, 2, NULL);
 *       if (lube_run(vm, "fn twice(x) => add(x, x)\nprint(twice(21))", "setup") != LUBE_OK)
 *           fprintf(stderr, "%s", lube_error_message(vm));
 *       LubeValue arg = lube_api()->number_value(5), result;
 *       if (lube_call_global(vm, "twice", 1, &arg, &result) == LUBE_OK)
 *           printf("%g\n", lube_api()->number(result));       // 10
 *       lube_api()->release(result);
 *       lube_close(vm);
 *   }
 *
 * Build it with Lube's library: `make lib` makes liblube.a (Lube's sources
 * but its command line), then `cc host.c -I lube/src lube/liblube.a -lm
 * -lpthread` (and -ldl on Linux, -lws2_32 on Windows when the plugins are
 * inside). From C++, include this header as it is (it is extern "C"), or
 * lube_embed.hpp for classes over it.
 *
 * Values are LubeValue, read and made through lube_api(): the same functions
 * plugins use (lube_plugin.h). A value a call gives you is yours: release it
 * when done (lube_api()->release). Code runs as files do, one after another
 * in the same interpreter: what one defines, the next can use.
 *
 * Errors come back as LUBE_ERROR, with lube_error_message(vm) saying what
 * (as Lube would print it), and never end the program. A script's exit(n)
 * returns LUBE_EXIT, with lube_exit_code(vm). Only running out of memory
 * ends the process (after lube_on_fatal's function, if there is one).
 *
 * One interpreter is used by one thread at a time. Several can live side by
 * side, on different threads; a program's isolates (spawn) have threads of
 * their own.
 */
#ifndef LUBE_EMBED_H
#define LUBE_EMBED_H

#include "lube_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LUBE_OK = 0,     /* it ran (or the call returned) */
    LUBE_ERROR = 1,  /* a syntax error, or an error nothing caught: lube_error_message(vm) */
    LUBE_EXIT = 2    /* it called exit(): lube_exit_code(vm) */
};

/* --- an interpreter --- */
LubeVM *lube_open(void);                     /* with every built-in; `args` is empty */
void lube_close(LubeVM *vm);                 /* (waits for the isolates it started) */
const LubeAPI *lube_api(void);               /* reading and making values (lube_plugin.h) */

/* --- running code --- */
/* Runs code as a file named `name` (for messages; NULL: "<host>"). Packages
 * are found from the current folder, and LUBE_PATH. */
int lube_run(LubeVM *vm, const char *code, const char *name);
int lube_run_script(LubeVM *vm, const char *path);  /* a file (its packages from its folder) */
const char *lube_error_message(LubeVM *vm);  /* the last error, with its line and traceback ("" if none) */
int lube_exit_code(LubeVM *vm);              /* what exit() was given, after LUBE_EXIT */

/* --- its globals and functions --- */
int lube_get(LubeVM *vm, const char *name, LubeValue *out);  /* 1 with the global's value (yours), 0 if none */
int lube_set(LubeVM *vm, const char *name, LubeValue value);  /* a global var, made or replaced: takes value */
/* Calls fn (a Lube function, or a built-in) with argc arguments (borrowed):
 * the result goes in *result (yours; result may be NULL to drop it). */
int lube_call_value(LubeVM *vm, LubeValue fn, int argc, const LubeValue *argv, LubeValue *result);
int lube_call_global(LubeVM *vm, const char *name, int argc, const LubeValue *argv, LubeValue *result);

/* --- yours, for it --- */
/* A global function name(...) that calls fn, which finds `data` with
 * lube_api()->function_data(vm). max_args -1 takes any number. */
void lube_function(LubeVM *vm, const char *name, LubeFn fn, int min_args, int max_args, void *data);
/* A plugin compiled into the program (made with LUBE_PLUGIN(name), with
 * types and methods): `import name` finds it, before any file. */
void lube_plugin_add(const char *name, int (*init)(struct LubePlugin *plugin, const struct LubeAPI *api));

/* --- the whole process --- */
/* Where what Lube prints goes: write(text, len, is_error, data). Without
 * one, what programs print goes to stdout, and errors only to
 * lube_error_message. Output and messages from every interpreter come here. */
void lube_set_output(void (*write)(const char *text, size_t len, int is_error, void *data), void *data);
void lube_on_fatal(void (*fn)(const char *message));  /* before running out of memory ends the process */

#ifdef __cplusplus
}
#endif

#endif
