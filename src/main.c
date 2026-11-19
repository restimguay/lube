/*
 * main.c - the `lube` command: run a script, evaluate code, or start the REPL.
 */
#include "lube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fprintf(out,
            "Lube %s - a small, smooth scripting language\n"
            "\n"
            "usage:\n"
            "  lube                  start the interactive REPL\n"
            "  lube FILE [ARGS...]   run a Lube script (ARGS are available as `args`)\n"
            "  lube -e CODE          run CODE and print the value of the last expression\n"
            "  lube -                read a script from standard input\n"
            "  lube --check FILE     report syntax errors without running anything (FILE may be -)\n"
            "  lube --debug FILE [ARGS...]\n"
            "                        run FILE in the debugger (h at its prompt lists the commands)\n"
            "  lube --hot FILE [ARGS...]\n"
            "                        run FILE, applying changes to its files while it runs\n"
            "  lube --build FILE [-o OUT]\n"
            "                        build FILE and its imports into a standalone executable\n"
            "  lube -v, --version    print the version\n"
            "  lube -h, --help       show this help\n",
            LUBE_VERSION);
}

static void set_args(Interp *I, int argc, char **argv, int first) {
    ObjList *list = new_list();
    for (int i = first; i < argc; i++) list_push(list, OBJ_VAL(new_string(argv[i], strlen(argv[i]))));
    lube_define(I, "args", OBJ_VAL(list));
}

/* Reads one line (without the newline). Returns 0 at end of input. */
static int read_line(StrBuf *line) {
    line->len = 0;
    int c, got = 0;
    while ((c = getchar()) != EOF) {
        got = 1;
        if (c == '\n') break;
        sb_putc(line, (char)c);
    }
    if (line->data) line->data[line->len] = '\0';
    return got;
}

/* > 0 while brackets or a string are still open: the REPL keeps reading. */
static int open_brackets(const char *s, size_t len) {
    const char *p = s, *end = s + len;
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '#' || (c == '/' && p + 1 < end && p[1] == '/')) {
            while (p < end && *p != '\n') p++;
        } else if (c == '"' || c == '\'') {
            int too_deep;
            p = skip_string_lit(p, end, &too_deep);
            if (!p) return too_deep ? 0 : 1;  /* too deep: let the lexer report it */
            continue;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            depth--;
        }
        p++;
    }
    return depth;
}

static void repl(Interp *I) {
    printf("Lube %s  (type exit() or press Ctrl-D to quit)\n", LUBE_VERSION);
    StrBuf code, line;
    sb_init(&code);
    sb_init(&line);
    for (;;) {
        fputs(code.len ? "... " : ">>> ", stdout);
        fflush(stdout);
        if (!read_line(&line)) {
            putchar('\n');
            break;
        }
        sb_putn(&code, line.data ? line.data : "", line.len);
        sb_putc(&code, '\n');
        if (open_brackets(code.data, code.len) > 0) continue;
        Source *src = lube_add_source(I, "<repl>", xstrndup(code.data, code.len), code.len);
        lube_run_source(I, src, 1);
        fflush(stdout);
        code.len = 0;
    }
    sb_free(&code);
    sb_free(&line);
}

static char *read_stdin(size_t *len) {
    StrBuf sb;
    sb_init(&sb);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) sb_putn(&sb, buf, n);
    if (!sb.data) sb_putn(&sb, "", 0);
    *len = sb.len;
    return sb.data;
}

/* LUBE_MAX_ITEMS / LUBE_MAX_BYTES can lower a size limit (never raise it). */
static void lower_limit(const char *name, size_t *limit) {
    const char *s = getenv(name);
    if (!s) return;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end || v == 0 || v >= *limit) {
        fprintf(stderr, "lube: ignoring %s=%s (expected a whole number below %lu)\n", name, s,
                (unsigned long)*limit);
        return;
    }
    *limit = (size_t)v;
}

int main(int argc, char **argv) {
    char stack_marker;  /* the interpreter measures recursion depth from here */
    lower_limit("LUBE_MAX_ITEMS", &lube_max_items);
    lower_limit("LUBE_MAX_BYTES", &lube_max_bytes);
    Interp *I = lube_new(&stack_marker);
    int bundled = lube_run_bundle(I, argv[0], argc, argv);  /* a built program runs itself */
    int status = bundled >= 0 ? bundled : 0;

    if (bundled >= 0) {
        /* done: every argument went to the program */
    } else if (argc < 2) {
        set_args(I, argc, argv, argc);
        repl(I);
    } else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
    } else if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
        printf("lube %s\n", LUBE_VERSION);
    } else if (!strcmp(argv[1], "-e")) {
        if (argc < 3) {
            usage(stderr);
            status = 64;
        } else {
            set_args(I, argc, argv, 3);
            Source *src = lube_add_source(I, "<eval>", xstrndup(argv[2], strlen(argv[2])), strlen(argv[2]));
            status = lube_run_source(I, src, 1);
        }
    } else if (!strcmp(argv[1], "--check")) {
        if (argc != 3) {
            usage(stderr);
            status = 64;
        } else {
            int from_stdin = !strcmp(argv[2], "-");
            size_t len;
            char *text = from_stdin ? read_stdin(&len) : read_file(argv[2], &len);
            if (!text) {
                fprintf(stderr, "lube: cannot open file '%s'\n", argv[2]);
                status = 66;
            } else {
                status = lube_check_source(I, lube_add_source(I, from_stdin ? "<stdin>" : argv[2], text, len));
            }
        }
    } else if (!strcmp(argv[1], "--debug")) {
        if (argc < 3) {
            usage(stderr);
            status = 64;
        } else {
            lube_debug_terminal(I);
            set_args(I, argc, argv, 3);
            status = lube_run_file(I, argv[2]);
        }
    } else if (!strcmp(argv[1], "--hot")) {
        if (argc < 3) {
            usage(stderr);
            status = 64;
        } else {
            I->hot = 1;
            set_args(I, argc, argv, 3);
            status = lube_run_file(I, argv[2]);
        }
    } else if (!strcmp(argv[1], "--build")) {
        /* lube --build app.lube [-o app]: the output is named after the file by default */
        if (argc != 3 && !(argc == 5 && !strcmp(argv[3], "-o"))) {
            usage(stderr);
            status = 64;
        } else {
            StrBuf out;
            sb_init(&out);
            if (argc == 5) {
                sb_puts(&out, argv[4]);
            } else {
                const char *name = strrchr(argv[2], '/') ? strrchr(argv[2], '/') + 1 : argv[2];
                const char *dot = strrchr(name, '.');
                sb_putn(&out, name, dot && dot != name ? (size_t)(dot - name) : strlen(name));
#ifdef _WIN32
                sb_puts(&out, ".exe");
#endif
            }
            if (!strcmp(out.data, argv[2])) {
                fprintf(stderr, "lube: the executable would replace '%s': name it with -o\n", argv[2]);
                status = 64;
            } else {
                status = lube_build(I, argv[0], argv[2], out.data);
            }
            sb_free(&out);
        }
    } else if (!strcmp(argv[1], "-")) {
        set_args(I, argc, argv, 2);
        size_t len;
        char *text = read_stdin(&len);
        status = lube_run_source(I, lube_add_source(I, "<stdin>", text, len), 0);
    } else if (argv[1][0] == '-') {
        fprintf(stderr, "lube: unknown option '%s'\n\n", argv[1]);
        usage(stderr);
        status = 64;
    } else {
        set_args(I, argc, argv, 2);
        status = lube_run_file(I, argv[1]);
    }

    lube_wait_isolates();  /* the program ends when every isolate has */
    fflush(stdout);
    size_t leaked = lube_free(I);
#ifdef LUBE_CHECK_LEAKS
    /* every value must have been freed: there is nothing a collector could clean up later */
    if (leaked) {
        fprintf(stderr, "lube: leak check failed: %lu objects still alive\n", (unsigned long)leaked);
        return 99;
    }
#else
    (void)leaked;
#endif
    if (lube_isolates_leaked()) return 99;  /* an isolate reported a leak (only after a bug) */
    return status;
}
