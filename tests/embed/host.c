/*
 * host.c - Lube inside a C program (tests/cli.sh builds it with Lube's
 * sources and runs it): every part of lube_embed.h.
 */
#include "lube_embed.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static const LubeAPI *lube;

/* ---- functions of the program's, for Lube ---- */

static LubeValue add(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    if (lube->kind(argv[0]) != LUBE_NUMBER || lube->kind(argv[1]) != LUBE_NUMBER)
        return lube->error(vm, "add() wants Numbers, not %s and %s", lube->a_type(argv[0]), lube->a_type(argv[1]));
    return lube->number_value(lube->number(argv[0]) + lube->number(argv[1]));
}

static LubeValue count(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    (void)argv;
    int *calls = (int *)lube->function_data(vm);
    return lube->number_value(++*calls);
}

/* texts_of(a, b, ...): every argument's text ("?" for what is not text), read only after every pointer was taken */
static LubeValue texts_of(LubeVM *vm, int argc, const LubeValue *argv) {
    const char *text[8];
    char out[256];
    size_t n = 0;
    if (argc > 8) argc = 8;
    for (int i = 0; i < argc; i++) text[i] = lube->str(argv[i], NULL);
    out[0] = '\0';
    for (int i = 0; i < argc; i++) n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", i ? "|" : "", text[i] ? text[i] : "?");
    return lube->new_str(vm, out, n);
}

/* call_back(f, x): Lube's f(x), called from C while Lube is calling C */
static LubeValue call_back(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    LubeValue r;
    int status = lube_call_value(vm, argv[0], 1, &argv[1], &r);
    int *calls = (int *)lube->function_data(vm);  /* (still its own, after the call) */
    ++*calls;
    if (status != LUBE_OK) return lube->error(vm, "call_back: the call failed");
    return r;
}

/* careless(f): calls Lube's f() and returns 0 whatever happened (an exit() in f still ends the run) */
static LubeValue careless(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    lube_call_value(vm, argv[0], 0, NULL, NULL);
    return lube->number_value(0);
}

/* ---- a plugin of the program's: import game ---- */

typedef struct {
    double x, y;
} Point;

static LubeType *PointType;

static int point_describe(const void *data, char *buf, size_t size) {
    const Point *p = (const Point *)data;
    return snprintf(buf, size, "Point(%g, %g)", p->x, p->y);
}

static LubeValue make_point(LubeVM *vm, double x, double y) {
    void *data;
    LubeValue v = lube->new_foreign(vm, PointType, &data);
    ((Point *)data)->x = x;
    ((Point *)data)->y = y;
    return v;
}

static LubeValue point_new(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    return make_point(vm, lube->number(argv[0]), lube->number(argv[1]));
}

static LubeValue point_moved(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    const Point *p = (const Point *)lube->foreign(argv[0], PointType);
    return make_point(vm, p->x + lube->number(argv[1]), p->y + lube->number(argv[2]));
}

static LubeValue point_x(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)vm;
    (void)argc;
    return lube->number_value(((const Point *)lube->foreign(argv[0], PointType))->x);
}

static LubeValue origin(LubeVM *vm, int argc, const LubeValue *argv) {
    (void)argc;
    (void)argv;
    return make_point(vm, 0, 0);
}

LUBE_PLUGIN(game) {
    PointType = api->type(plugin, "Point", sizeof(Point), NULL, point_describe);
    api->constructor(PointType, point_new, 2, 2);
    api->method(PointType, "moved", point_moved, 3, 3);
    api->method(PointType, "x", point_x, 1, 1);
    api->function(plugin, "origin", origin, 0, 0);
    return LUBE_PLUGIN_API;
}

/* ---- what Lube prints, through the program ---- */

static void show(const char *text, size_t len, int is_error, void *data) {
    printf("%s%.*s", is_error ? (const char *)data : "", (int)len, text);
}

static void last_words(const char *message) { fprintf(stderr, "lube says: %s\n", message); }

/* The first line of the last error. */
static void first_line(LubeVM *vm) {
    const char *m = lube_error_message(vm);
    const char *end = strchr(m, '\n');
    printf("%.*s\n", end ? (int)(end - m) : (int)strlen(m), m);
}

/* ---- a thread with a small stack: deep recursion is an error there, not a crash ---- */

static void deep_recursion(void) {
    LubeVM *vm = lube_open();
    int s = lube_run(vm,
                     "fn deep(n) {\n"
                     "    if n == 0 { return 0 }\n"
                     "    return map([n - 1], deep)[0] + 1  // (each level: C calling Lube calling C)\n"
                     "}\n"
                     "print(deep(1000000))\n",
                     "deep");
    printf("on a 256 KB stack: status %d, ", s);
    first_line(vm);
    lube_close(vm);
}

#ifdef _WIN32
static DWORD WINAPI small_stack(LPVOID arg) {
    (void)arg;
    deep_recursion();
    return 0;
}
#else
static void *small_stack(void *arg) {
    (void)arg;
    deep_recursion();
    return NULL;
}
#endif

int main(void) {
    lube = lube_api();
    int calls = 0, s;
    LubeVM *vm = lube_open();
    LubeValue v, r;

    printf("-- running code\n");
    s = lube_run(vm, "print(\"hello from Lube\", 1 + 2)\nvar score = 10\nfn twice(x) => x * 2\n", "setup");
    printf("status %d\n", s);
    s = lube_run(vm, "score += 5\nprint(\"score is\", score, twice(score), args)", "more");
    printf("status %d\n", s);

    printf("-- globals\n");
    if (lube_get(vm, "score", &v)) printf("score = %g (%s)\n", lube->number(v), lube->type_name(v));
    lube->release(v);
    printf("a global that is not there: %d\n", lube_get(vm, "nope", &v));
    lube_set(vm, "name", lube->new_str(vm, "Ada", 3));
    lube_set(vm, "speed", lube->number_value(2.5));
    lube_run(vm, "print(name, speed * 2, type(name))\nspeed += 1", "globals");
    lube_get(vm, "speed", &v);
    printf("speed = %g\n", lube->number(v));
    lube->release(v);

    printf("-- calling Lube\n");
    v = lube->number_value(21);
    s = lube_call_global(vm, "twice", 1, &v, &r);
    printf("twice(21): status %d, %g\n", s, lube->number(r));
    lube->release(r);
    LubeValue len_fn, word = lube->new_str(vm, "hello", 5);
    lube_get(vm, "len", &len_fn);  /* (a built-in) */
    s = lube_call_value(vm, len_fn, 1, &word, &r);
    printf("len(\"hello\"): status %d, %g\n", s, lube->number(r));
    lube->release(r);
    lube->release(word);
    s = lube_call_value(vm, len_fn, 0, NULL, NULL);
    printf("len(): status %d\n%s", s, lube_error_message(vm));
    lube->release(len_fn);
    s = lube_call_global(vm, "twice", 1, &v, NULL);
    printf("twice(21), the result dropped: status %d\n", s);
    s = lube_call_global(vm, "nothing_here", 0, NULL, NULL);
    printf("status %d: %s", s, lube_error_message(vm));
    s = lube_call_global(vm, "score", 0, NULL, NULL);
    printf("status %d: %s", s, lube_error_message(vm));
    LubeValue many[10];
    for (int i = 0; i < 10; i++) many[i] = lube->number_value(i);
    lube_run(vm, "fn total(a, b, c, d, e, f, g, h, i, j) => a + b + c + d + e + f + g + h + i + j", NULL);
    s = lube_call_global(vm, "total", 10, many, &r);  /* (more arguments than fit on its stack) */
    printf("total(0, ..., 9): status %d, %g\n", s, lube->number(r));
    lube->release(r);

    printf("-- text, short and long\n");
    /* A str of a few bytes is held in its value, not in an object. What a call or a global gives the program is
     * text it can point at all the same, until it lets go of the value, whatever it runs meanwhile; and so is an
     * item of a list, or a value of an object, for as long as it stays there. */
    lube_run(vm,
             "fn tag(n) => \"t{n}\"\n"
             "fn sentence(n) => \"a sentence that is longer: {n}\"\n"
             "var label = \"ok\"\n"
             "var names = [\"a\", \"bb\", \"a longer name here\"]\n"
             "var info = {id: \"x1\", note: \"a note that is longer\"}",
             "text");
    LubeValue made[4], label, names, info, item;
    const char *held[4], *name_text[3];
    for (int i = 0; i < 4; i++) {
        v = lube->number_value(i);
        lube_call_global(vm, i % 2 ? "sentence" : "tag", 1, &v, &made[i]);
        held[i] = lube->str(made[i], NULL);
    }
    lube_get(vm, "label", &label);
    const char *label_text = lube->str(label, NULL);
    lube_get(vm, "names", &names);
    lube_get(vm, "info", &info);
    for (size_t i = 0; i < 3; i++) name_text[i] = lube->str(lube->list_get(names, i), NULL);
    lube->object_get(vm, info, "id", &item);
    const char *id = lube->str(item, NULL);
    lube_run(vm, "label = \"changed\"\nprint(tag(9), names, info, names[0] == \"a\", info.id + \"!\")", "text2");
    printf("%s | %s | %s | %s | %s | %s %s %s | %s\n", held[0], held[1], held[2], held[3], label_text, name_text[0],
           name_text[1], name_text[2], id);
    for (int i = 0; i < 4; i++) lube->release(made[i]);
    lube->release(label);
    lube->release(names);
    lube->release(info);
    LubeValue mine = lube->new_str(vm, "mine", 4);  /* (one the program makes, and reads back) */
    size_t mine_len;
    const char *mine_text = lube->str(mine, &mine_len);
    printf("%s %d\n", mine_text, (int)mine_len);
    lube->release(mine);
    lube_function(vm, "texts_of", texts_of, 0, -1, NULL);
    lube_run(vm, "print(texts_of(\"a\", \"bb\", \"a longer argument here\", \"a\", 5, \"{label}!\"))", "text3");

    printf("-- functions of the program's\n");
    lube_function(vm, "add", add, 2, 2, NULL);
    lube_function(vm, "count", count, 0, 0, &calls);
    lube_function(vm, "call_back", call_back, 2, 2, &calls);
    lube_function(vm, "careless", careless, 1, 1, NULL);
    lube_run(vm,
             "print(add(2, 3), count(), count(), count())\n"
             "print(call_back(fn(x) => x + 100, 1), count())\n"
             "try { add(1, \"x\") } catch e { print(\"caught:\", e) }\n"
             "print(signature(add).builtin, add is Function)",
             "functions");
    printf("calls counted in C: %d\n", calls);

    printf("-- errors\n");
    s = lube_run(vm, "print(\"before\")\nlet broken = (1 +", "bad_syntax");
    printf("status %d\n%s", s, lube_error_message(vm));
    s = lube_run(vm, "fn inner() { return 1 / [] }\nfn outer() => inner()\nouter()", "bad_run");
    printf("status %d\n%s", s, lube_error_message(vm));
    s = lube_run(vm, "print(\"fine again\")", NULL);
    printf("status %d, message \"%s\"\n", s, lube_error_message(vm));
    s = lube_run(vm, "print(call_back(fn(x) => x / [], 1))", "fails_inside");
    printf("status %d\n%s", s, lube_error_message(vm));

    printf("-- exit\n");
    s = lube_run(vm, "try { exit(3) } catch e { print(\"not caught\") }\nprint(\"not reached\")", "exits");
    printf("status %d, code %d, message \"%s\"\n", s, lube_exit_code(vm), lube_error_message(vm));
    s = lube_run(vm, "try { call_back(fn(x) { exit(x) }, 7) } catch e { print(\"not caught\") }\nprint(\"no\")", "exits_inside");
    printf("status %d, code %d\n", s, lube_exit_code(vm));
    s = lube_run(vm, "try { careless(fn() { exit(8) }) } catch e { print(\"not caught\") }\nprint(\"no\")", "careless");
    printf("status %d, code %d\n", s, lube_exit_code(vm));
    lube_run(vm, "fn quit() { exit(4) }", NULL);
    s = lube_call_global(vm, "quit", 0, NULL, NULL);
    printf("status %d, code %d\n", s, lube_exit_code(vm));
    s = lube_run(vm, "print(\"still here\")", NULL);
    printf("status %d\n", s);

    printf("-- a plugin of the program's\n");
    lube_plugin_add("game", lube_plugin_init_game);
    lube_run(vm,
             "import game\n"
             "let p = game.Point(1, 2)\n"
             "print(p, p.moved(3, 4), p.moved(3, 4).x(), type(p), p is game.Point, game.origin())",
             "plugin");

    printf("-- a script file\n");
    s = lube_run_script(vm, "tests/embed/script.lube");
    printf("status %d\n", s);
    s = lube_run_script(vm, "tests/embed/no_such.lube");
    printf("status %d: %s", s, lube_error_message(vm));

    printf("-- output through the program\n");
    lube_set_output(show, "[error] ");
    lube_run(vm, "print(\"through show\")\nprint(1 / 0)", "shown");
    lube_set_output(NULL, NULL);

    printf("-- isolates\n");
    lube_run(vm, "fn square(n: Number): Number => n * n\nprint(parallel_map([1, 2, 3], square), spawn(square, 9).join())",
             "isolates");

    printf("-- two interpreters\n");
    LubeVM *other = lube_open();
    lube_run(other, "var score = 99\nprint(score)", NULL);
    lube_run(vm, "print(score)", NULL);
    lube_close(other);

    printf("-- a small stack\n");
    fflush(stdout);
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 256 * 1024, small_stack, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_create(&t, &attr, small_stack, NULL);
    pthread_join(t, NULL);
    pthread_attr_destroy(&attr);
#endif

    lube_on_fatal(last_words);
    lube_close(vm);
    lube_close(NULL);
    printf("-- closed\n");
    return 0;
}
