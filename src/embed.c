/*
 * embed.c - Lube inside another program: what lube_embed.h promises.
 *
 * A host program's interpreter is a plain one with a Hosted beside it: the
 * text of its last error (caught from what Lube would have printed), and the
 * program's functions. Each call into it from the program measures the C
 * stack the thread has left, from there: the program's threads are not the
 * 64 MB ones the lube command gives itself. exit() raises an error that no
 * catch stops (interp.c), which comes back here as LUBE_EXIT.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* pthread_getattr_np: where this thread's stack ends */
#endif
#include "lube.h"
#include "lube_embed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define TokenType WinTokenType  /* winnt.h has a TokenType of its own */
#include <windows.h>
#undef TokenType
#else
#include <pthread.h>
#endif

typedef struct {
    StrBuf error;            /* the last error, as Lube says it */
    const void **fns;        /* the program's functions (plugin.c's), freed with the interpreter */
    int nfns;
    int depth;               /* calls in from the program, one inside another (through its functions) */
} Hosted;

static void (*host_write)(const char *text, size_t len, int is_error, void *data);
static void *host_data;
static LUBE_THREAD_LOCAL Interp *current;  /* the interpreter this thread runs for the program */

/* Everything Lube prints, in a host program: an error goes to the
 * interpreter's message too. */
static void hosted_output(const char *text, size_t len, int is_err) {
    if (is_err && current) sb_putn(&((Hosted *)current->hosted)->error, text, len);
    if (host_write) host_write(text, len, is_err, host_data);
    else if (!is_err) fwrite(text, 1, len, stdout);
}

/* How many bytes of stack this thread has below `here`. */
static size_t stack_room(char *here) {
    char *low = NULL;
#if defined(__APPLE__)
    pthread_t t = pthread_self();
    low = (char *)pthread_get_stackaddr_np(t) - pthread_get_stacksize_np(t);
#elif defined(_WIN32)
    MEMORY_BASIC_INFORMATION m;  /* (the stack is one reservation: its base is its end) */
    if (VirtualQuery(here, &m, sizeof m)) low = (char *)m.AllocationBase;
#elif defined(__linux__)
    pthread_attr_t a;
    void *addr;
    size_t size;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        if (pthread_attr_getstack(&a, &addr, &size) == 0) low = (char *)addr;
        pthread_attr_destroy(&a);
    }
#endif
    if (!low || low >= here) return 256u * 1024u;  /* LCOV_EXCL_LINE: the system would not say: be modest */
    return (size_t)(here - low);
}

/* A call in from the program begins: the stack is measured from here (the
 * outermost one), and the last error forgotten. */
static Interp *enter(LubeVM *vm, char *here, Interp **was) {
    Interp *I = (Interp *)vm;
    Hosted *h = (Hosted *)I->hosted;
    if (h->depth++ == 0) {
        size_t room = stack_room(here), margin = room / 8 > 64u * 1024u ? room / 8 : 64u * 1024u;
        I->stack_base = here;
        I->stack_budget = room > margin ? room - margin : 0;
        I->exiting = 0;
    }
    h->error.len = 0;
    if (h->error.data) h->error.data[0] = '\0';
    *was = current;
    current = I;
    return I;
}

/* ...and ends: LUBE_OK, LUBE_ERROR or LUBE_EXIT. */
static int leave(Interp *I, Interp *was, int failed) {
    Hosted *h = (Hosted *)I->hosted;
    current = was;
    h->depth--;
    if (I->exiting) return LUBE_EXIT;  /* (it stays set until the program's outermost call ends) */
    return failed ? LUBE_ERROR : LUBE_OK;
}

LubeVM *lube_open(void) {
    char here;
    lube_hosted = 1;
    lube_output = hosted_output;
    Interp *I = lube_new(&here);  /* (each call in measures its own stack: this is only for now) */
    Hosted *h = (Hosted *)xmalloc(sizeof(Hosted));
    memset(h, 0, sizeof *h);
    sb_init(&h->error);
    I->hosted = h;
    ObjList *args = new_list();
    args->elem = TT_STRING;
    lube_define(I, "args", OBJ_VAL(args));
    return (LubeVM *)I;
}

void lube_close(LubeVM *vm) {
    if (!vm) return;
    Hosted *h = (Hosted *)((Interp *)vm)->hosted;
    lube_wait_isolates();
    lube_free((Interp *)vm);
    for (int i = 0; i < h->nfns; i++) lube_free_host_fn(h->fns[i]);  /* (after the natives that called them) */
    free(h->fns);
    sb_free(&h->error);
    free(h);
}

const LubeAPI *lube_api(void) { return lube_plugin_api(); }

int lube_run(LubeVM *vm, const char *code, const char *name) {
    char here;
    Interp *was, *I = enter(vm, &here, &was);
    size_t len = strlen(code);
    Source *src = lube_add_source(I, name ? name : "<host>", xstrndup(code, len), len);
    return leave(I, was, lube_run_source(I, src, 0) != 0);
}

int lube_run_script(LubeVM *vm, const char *path) {
    char here;
    Interp *was, *I = enter(vm, &here, &was);
    return leave(I, was, lube_run_file(I, path) != 0);
}

const char *lube_error_message(LubeVM *vm) {
    const Hosted *h = (const Hosted *)((Interp *)vm)->hosted;
    return h->error.data ? h->error.data : "";
}

int lube_exit_code(LubeVM *vm) { return ((Interp *)vm)->exit_code; }

int lube_get(LubeVM *vm, const char *name, LubeValue *out) {
    Interp *I = (Interp *)vm;
    Value v;
    if (!map_get(I->globals, OBJ_VAL(lube_intern(I, name, strlen(name))), &v) || v.type == VAL_LENT) return 0;
    *out = value_to_lube(retain(v));
    return 1;
}

int lube_set(LubeVM *vm, const char *name, LubeValue value) {
    char here;
    Interp *was, *I = enter(vm, &here, &was);
    int failed = lube_assign_global(I, lube_intern(I, name, strlen(name)), value_from_lube(value)) < 0;
    if (failed) lube_report_uncaught(I);  /* LCOV_EXCL_LINE: only a global lent out with & (by running code) */
    return leave(I, was, failed);
}

int lube_call_value(LubeVM *vm, LubeValue fn, int argc, const LubeValue *argv, LubeValue *result) {
    char here;
    Interp *was, *I = enter(vm, &here, &was);
    Value f = value_from_lube(fn), r;
    if (!IS_CALLABLE(f)) {
        sb_printf(&((Hosted *)I->hosted)->error, "error: lube_call: %s is not a function\n", a_type(f));
        return leave(I, was, 1);
    }
    Value small[8];
    memset(small, 0, sizeof small);  /* (zeroed: gcc would rather it were, with no arguments) */
    Value *args = argc <= 8 ? small : (Value *)xmalloc(sizeof(Value) * (size_t)argc);
    for (int i = 0; i < argc; i++) args[i] = value_from_lube(argv[i]);
    ObjNative *calling = I->calling;  /* (the program's function that called this finds its data after) */
    r = lube_call(I, f, argc, args);
    I->calling = calling;
    if (args != small) free(args);
    int failed = IS_ERR(r);
    if (failed) lube_report_uncaught(I);
    else if (result) *result = value_to_lube(r);
    else release(r);
    return leave(I, was, failed);
}

int lube_call_global(LubeVM *vm, const char *name, int argc, const LubeValue *argv, LubeValue *result) {
    LubeValue fn;
    if (!lube_get(vm, name, &fn)) {
        Hosted *h = (Hosted *)((Interp *)vm)->hosted;
        h->error.len = 0;
        sb_printf(&h->error, "error: lube_call: there is no global '%s'\n", name);
        return LUBE_ERROR;
    }
    int status = lube_call_value(vm, fn, argc, argv, result);
    release(value_from_lube(fn));
    return status;
}

void lube_function(LubeVM *vm, const char *name, LubeFn fn, int min_args, int max_args, void *data) {
    Interp *I = (Interp *)vm;
    Hosted *h = (Hosted *)I->hosted;
    Value native = lube_host_native(I, name, fn, min_args, max_args, data);
    h->fns = (const void **)xrealloc((void *)h->fns, sizeof(void *) * (size_t)(h->nfns + 1));
    h->fns[h->nfns++] = AS_NATIVE(native)->plugin_fn;
    lube_define(I, name, native);
}

void lube_set_output(void (*write)(const char *text, size_t len, int is_error, void *data), void *data) {
    host_write = write;
    host_data = data;
}

void lube_on_fatal(void (*fn)(const char *message)) { lube_fatal_hook = fn; }
