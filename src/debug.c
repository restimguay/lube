/*
 * debug.c - the debugger: breakpoints (with conditions, hit counts and log
 * messages), stepping, breaking on errors, frames, variables, and Lube
 * expressions evaluated in a paused frame. The terminal prompt (lube
 * --debug) is here; dap.c drives the same core for VS Code.
 *
 * The VM calls debug_hook before each instruction while I->dbg is set. The
 * debugger only looks closer when a frame reaches a new line, so a paused
 * program always stops at the start of a line, with every frame intact.
 */
#include "lube.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LUBE_RUNTIME  /* (a program built with --small has no debugger: see the end) */

/* Ctrl-C while the program runs under the terminal debugger: pause it. */
static volatile sig_atomic_t interrupted;
static void on_interrupt(int sig) {
    (void)sig;
    interrupted = 1;
}

enum { STEP_NONE, STEP_IN, STEP_OVER, STEP_OUT };
enum { CATCH_OFF, CATCH_UNCAUGHT, CATCH_ALL };

typedef struct {
    int id;
    char *file;             /* as given: a path, or the end of one (app.lube) */
    int line;
    char *cond;             /* stop only when this is true (NULL: always) */
    char *hit;              /* stop only on these hits: "5", ">= 5", "% 3" (NULL: every one) */
    char *log;              /* print this message instead of stopping (NULL: stop) */
    int hits;
    int temporary;          /* removed when it stops the program */
    FuncDecl *compiled;     /* cond or log, compiled for one function... */
    FuncDecl *compiled_for; /* ...this one */
} Breakpoint;

typedef struct Debugger {
    int step, step_depth;   /* STEP_*, and the call depth when the step began */
    int pause;              /* stop at the next line */
    int at_entry;           /* the first stop is "entry" */
    int catch_mode;         /* CATCH_*: stop where errors are raised */
    int busy;               /* the debugger is running Lube code itself: no stops */
    Breakpoint *bps;
    int nbps, capbps, next_id;
    int selected;           /* the frame the commands look at (0: the innermost) */
} Debugger;

/* ---------- frames and variables ---------- */

static Frame *nth_frame(Interp *I, int n) {
    Frame *f = I->frame;
    while (f && n-- > 0) f = f->prev;
    return f;
}

static int count_frames(Interp *I) {
    int n = 0;
    for (Frame *f = I->frame; f; f = f->prev) n++;
    return n;
}

/* The variables code paused in frame f can see: its locals that hold a
 * value at this point (the innermost of a name), then what it captured.
 * Returns how many (up to max). */
static int frame_vars(Frame *f, DebugVar *out, int max) {
    FuncDecl *d = f->proto;
    int n = 0, pc = (int)(f->pc - d->code.code);
    for (int i = 0; i < d->code.nlocals; i++) {
        LocalInfo *l = &d->code.locals[i];
        if (pc < l->start || pc >= l->end) continue;
        int k = 0;
        while (k < n && out[k].name != l->name) k++;
        if (k == n && n == max) continue;
        if (k < n && out[k].index != l->slot) {  /* the same name again, further in: it shadows the other */
            for (int j = k; j < n - 1; j++) out[j] = out[j + 1];
            k = --n;
        }
        out[k].name = l->name;
        out[k].kind = VAR_LOCAL;
        out[k].index = l->slot;
        if (k == n) n++;
    }
    for (int i = 0; f->fn && i < d->ncaptures && n < max; i++) {
        int k = 0;
        while (k < n && out[k].name != d->capnames[i]) k++;
        if (k < n) continue;
        out[n].name = d->capnames[i];
        out[n].kind = VAR_CAPTURE;
        out[n].index = i;
        n++;
    }
    if (f->block) {  /* an `on` block running on its function's frame: it sees that frame's variables too */
        Frame *owner = f->prev;
        while (owner->base != f->base || owner->block) owner = owner->prev;
        DebugVar more[256];
        int m = frame_vars(owner, more, 256);
        for (int i = 0; i < m && n < max; i++) {
            int k = 0;
            while (k < n && out[k].name != more[i].name) k++;
            if (k == n) out[n++] = more[i];
        }
    }
    return n;
}

static Value var_value(Interp *I, Frame *f, DebugVar *v) {
    return v->kind == VAR_LOCAL ? I->stack[f->base + v->index] : f->fn->captures[v->index];
}

/* ---------- expressions in a paused frame ---------- */

/* Compiles `text`, an expression, as a function to run in frame f (the
 * frame's variables are its captures). NULL if it has errors (reported). */
static FuncDecl *compile_in(Interp *I, Frame *f, const char *text) {
    Source *src = lube_add_source(I, "<debugger>", xstrndup(text, strlen(text)), strlen(text));
    src->strict = 0;
    Arena *arena;
    Node *program = parse_program(I, src, &arena);
    if (!program) return NULL;
    NodeList *stmts = &program->as.block.stmts;
    if (stmts->count != 1 || stmts->items[0]->kind != N_EXPR) {
        report_error(src, 1, 0, "error", "the debugger evaluates one expression");
        arena_free(arena);
        return NULL;
    }
    FuncDecl *d = arena_alloc(arena, sizeof(FuncDecl));
    d->name = lube_intern(I, "<debugger>", 10);
    d->line = 1;
    d->src = src;
    d->is_arrow = 1;
    d->body = stmts->items[0]->as.expr;
    DebugVar vars[256];
    int n = frame_vars(f, vars, 256);
    if (!resolve_debug(I, d, src, arena, f->src, vars, n)) {
        arena_free(arena);
        return NULL;
    }
    lube_keep_arena(I, arena);
    return compile_debug(I, d, arena);
}

/* Runs a compiled expression in frame f. Returns NULL with the value in
 * *out (a new reference), or the error message (malloc'd). The program's
 * own pending error, if any, is kept. */
static char *run_in(Interp *I, Frame *f, FuncDecl *d, Value *out) {
    Debugger *D = I->dbg;
    Value saved = I->error_val;
    int saved_line = I->error_line;
    Source *saved_src = I->error_src;
    I->error_val = NIL_VAL;
    D->busy++;
    Value fn = lube_closure_in(I, d, f);
    Value r = lube_call(I, fn, 0, NULL);
    release(fn);
    D->busy--;
    char *err = NULL;
    if (IS_ERR(r)) {
        ObjString *s = value_to_string(I->error_val);
        err = xstrndup(s->chars, s->length);
        release(OBJ_VAL(s));
        release(I->error_val);
    } else if (IS_NIL(r)) {
        err = xstrndup("the expression gives no value", 29);
    }
    I->error_val = saved;
    I->error_line = saved_line;
    I->error_src = saved_src;
    *out = r;
    return err;
}

/* Evaluates `text` in frame f: NULL with *out set, or an error message. */
static char *eval_in(Interp *I, Frame *f, const char *text, Value *out) {
    I->dbg->busy++;
    FuncDecl *d = compile_in(I, f, text);
    I->dbg->busy--;
    if (!d) return xstrndup("the expression has errors (see above)", 37);
    return run_in(I, f, d, out);
}

/* ---------- breakpoints ---------- */

static int same_file(const char *given, const char *path) {
    size_t n = strlen(given), m = strlen(path);
    return !strcmp(given, path) || (m > n && path[m - n - 1] == '/' && !strcmp(path + m - n, given));
}

static Breakpoint *add_breakpoint(Debugger *D, const char *file, int line) {
    if (D->nbps == D->capbps) {
        D->capbps = D->capbps ? D->capbps * 2 : 8;
        D->bps = xrealloc(D->bps, sizeof(Breakpoint) * (size_t)D->capbps);
    }
    Breakpoint *b = &D->bps[D->nbps++];
    memset(b, 0, sizeof *b);
    b->id = ++D->next_id;
    b->file = xstrndup(file, strlen(file));
    b->line = line;
    return b;
}

static void delete_breakpoint(Debugger *D, int i) {
    Breakpoint *b = &D->bps[i];
    free(b->file);
    free(b->cond);
    free(b->hit);
    free(b->log);
    memmove(b, b + 1, sizeof(Breakpoint) * (size_t)(D->nbps - i - 1));
    D->nbps--;
}

/* Does hit number n satisfy a hit condition: "5" or "== 5" (the 5th),
 * ">= 5", "> 5", "% 3" (every 3rd)? */
static int hit_ok(const char *cond, int n) {
    while (*cond == ' ') cond++;
    char op[3] = {0};
    int k = 0;
    while (k < 2 && strchr("=<>%", cond[k])) {
        op[k] = cond[k];
        k++;
    }
    int want = atoi(cond + k);
    if (!strcmp(op, ">=")) return n >= want;
    if (!strcmp(op, ">")) return n > want;
    if (!strcmp(op, "<=")) return n <= want;
    if (!strcmp(op, "<")) return n < want;
    if (!strcmp(op, "%")) return want > 0 && n % want == 0;
    return n == want;  /* "5" or "== 5" */
}

static void print_out(Interp *I, const char *text);

/* The breakpoint's condition or log message, compiled for frame f's code. */
static FuncDecl *compiled(Interp *I, Breakpoint *b, Frame *f) {
    if (b->compiled_for == f->proto) return b->compiled;
    StrBuf sb;
    sb_init(&sb);
    if (b->log) {  /* the message, as an interpolated string */
        sb_putc(&sb, '"');
        for (const char *p = b->log; *p; p++) {
            if (*p == '"' || *p == '\\') sb_putc(&sb, '\\');
            sb_putc(&sb, *p);
        }
        sb_putc(&sb, '"');
    } else {
        sb_puts(&sb, b->cond);
    }
    I->dbg->busy++;
    b->compiled = compile_in(I, f, sb.data);
    I->dbg->busy--;
    b->compiled_for = f->proto;
    sb_free(&sb);
    return b->compiled;
}

/* A frame reached a line: does a breakpoint there stop it (returns it),
 * after counting hits, checking conditions and printing log messages? */
static Breakpoint *breakpoint_at(Interp *I, Frame *f, int line) {
    Debugger *D = I->dbg;
    for (int i = 0; i < D->nbps; i++) {
        Breakpoint *b = &D->bps[i];
        if (b->line != line || !f->src || !same_file(b->file, f->src->path)) continue;
        Value v;
        if (b->cond) {
            FuncDecl *d = compiled(I, b, f);
            char *err = d ? run_in(I, f, d, &v) : xstrndup("the condition has errors (see above)", 36);
            if (err) {  /* a broken condition stops, so you can see why */
                StrBuf sb;
                sb_init(&sb);
                sb_printf(&sb, "breakpoint %d: the condition failed: %s", b->id, err);
                print_out(I, sb.data);
                sb_free(&sb);
                free(err);
                return b;
            }
            int t = is_truthy(v);
            release(v);
            if (!t) continue;
        }
        b->hits++;
        if (b->hit && !hit_ok(b->hit, b->hits)) continue;
        if (b->log) {
            FuncDecl *d = compiled(I, b, f);
            char *err = d ? run_in(I, f, d, &v) : xstrndup("the message has errors (see above)", 34);
            if (err) {
                StrBuf sb;
                sb_init(&sb);
                sb_printf(&sb, "logpoint %d: %s", b->id, err);
                print_out(I, sb.data);
                sb_free(&sb);
                free(err);
            } else {
                print_out(I, AS_STRING(v)->chars);
                release(v);
            }
            continue;
        }
        return b;
    }
    return NULL;
}

/* ---------- stopping ---------- */

static void terminal_stop(Interp *I, const char *reason, const char *text);

static void stop(Interp *I, const char *reason, const char *text) {
    Debugger *D = I->dbg;
    D->step = STEP_NONE;
    D->selected = 0;
    terminal_stop(I, reason, text);
}

void debug_hook(Interp *I) {
    Debugger *D = I->dbg;
    Frame *f = I->frame;
    if (D->busy) return;
    int line = lube_frame_line(f);
    if (line == f->dbg_line) return;  /* still on the same line */
    f->dbg_line = line;
    Breakpoint *b = breakpoint_at(I, f, line);
    if (b) {
        char text[64];
        snprintf(text, sizeof text, "%d", b->id);
        if (b->temporary) delete_breakpoint(D, (int)(b - D->bps));
        stop(I, "breakpoint", text);
    } else if (D->at_entry) {
        D->at_entry = 0;
        stop(I, "entry", NULL);
    } else if (D->pause || interrupted) {
        D->pause = 0;
        interrupted = 0;
        stop(I, "pause", NULL);
    } else if (D->step == STEP_IN || (D->step == STEP_OVER && I->depth <= D->step_depth) ||
               (D->step == STEP_OUT && I->depth < D->step_depth)) {
        stop(I, "step", NULL);
    }
}

/* Will a try in some frame catch an error raised here? */
static int caught(Interp *I) {
    for (Frame *f = I->frame; f; f = f->prev) {
        int at = (int)(f->pc - f->proto->code.code);
        for (int i = 0; i < f->proto->code.nhandlers; i++)
            if (at >= f->proto->code.handlers[i].start && at < f->proto->code.handlers[i].end) return 1;
    }
    return 0;
}

void debug_error(Interp *I) {
    Debugger *D = I->dbg;
    if (D->busy || D->catch_mode == CATCH_OFF || !I->frame) return;
    if (D->catch_mode == CATCH_UNCAUGHT && caught(I)) return;
    ObjString *msg = value_to_string(I->error_val);
    stop(I, "error", msg->chars);
    release(OBJ_VAL(msg));
}

void debug_left_frame(Interp *I) {
    Debugger *D = I->dbg;  /* stepping: stop again in the caller (not after the debugger's own calls) */
    if (D->step != STEP_NONE && !D->busy && I->frame) I->frame->dbg_line = -1;
}

/* ---------- the terminal prompt ---------- */

static void print_out(Interp *I, const char *text) {
    (void)I;
    fflush(stdout);
    printf("%s\n", text);
}

/* Line `line` of a source, or NULL (with its length in *len). */
static const char *source_line(Source *src, int line, int *len) {
    if (!src || !src->text || line < 1) return NULL;
    const char *p = src->text, *end = src->text + src->length;
    for (int cur = 1; cur < line && p < end; p++)
        if (*p == '\n') cur++;
    if (p >= end) return NULL;
    const char *e = p;
    while (e < end && *e != '\n') e++;
    *len = (int)(e - p);
    return p;
}

static void show_frame(Interp *I, Frame *f, int marker) {
    int line = lube_frame_line(f), len;
    printf("%s at %s (%s:%d)\n", marker ? "=>" : "  ", f->name, f->src ? f->src->path : "?", line);
    const char *text = source_line(f->src, line, &len);
    if (text) printf("%5d | %.*s\n", line, len, text);
    (void)I;
}

static void show_value(Interp *I, const char *name, Value v) {
    (void)I;
    ObjString *s = value_repr(v);
    if (name) printf("%s = %s\n", name, s->chars);
    else printf("%s\n", s->chars);
    release(OBJ_VAL(s));
}

static void list_source(Frame *f) {
    int line = lube_frame_line(f), len;
    for (int l = line - 5 < 1 ? 1 : line - 5; l <= line + 5; l++) {
        const char *text = source_line(f->src, l, &len);
        if (!text) break;
        printf("%s%4d | %.*s\n", l == line ? "=>" : "  ", l, len, text);
    }
}

/* file:line or line (in the selected frame's file). 0 if it is neither. */
static int parse_location(Interp *I, const char *arg, char *file, size_t size, int *line) {
    const char *colon = strrchr(arg, ':');
    Frame *f = nth_frame(I, I->dbg->selected);
    if (colon) {
        snprintf(file, size, "%.*s", (int)(colon - arg), arg);
        arg = colon + 1;
    } else {
        snprintf(file, size, "%s", f && f->src ? f->src->path : "");
    }
    char *end;
    long n = strtol(arg, &end, 10);
    if (end == arg || n < 1) return 0;
    *line = (int)n;
    return 1;
}

static const char HELP[] =
    "  c, continue        run to the next breakpoint\n"
    "  n, next            the next line, over calls\n"
    "  s, step            the next line, into calls\n"
    "  o, out             until this function returns\n"
    "  b [file:]line [if cond]   a breakpoint (tb: one that is removed when hit)\n"
    "  b [file:]line hits n      one that stops on the nth hit (or: >= n, % n)\n"
    "  log [file:]line msg       print msg (with {expressions}) there, without stopping\n"
    "  bl                 list the breakpoints;  d [n]: delete one, or all\n"
    "  catch all|uncaught|off    stop where errors are raised\n"
    "  p expr             evaluate an expression here\n"
    "  locals, globals    show variables\n"
    "  bt                 the calls that led here;  up, down, frame n: look at another\n"
    "  l                  the source around here\n"
    "  q                  quit the program\n";

static void terminal_stop(Interp *I, const char *reason, const char *text) {
    Debugger *D = I->dbg;
    fflush(stdout);
    if (!strcmp(reason, "breakpoint")) printf("breakpoint %s\n", text);
    else if (!strcmp(reason, "error")) printf("error raised: %s\n", text);
    else if (!strcmp(reason, "pause")) printf("paused\n");
    show_frame(I, I->frame, 0);
    char buf[1024];
    for (;;) {
        printf("(lube) ");
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {  /* no more commands: run on without the debugger */
            printf("\n");
            I->dbg = NULL;
            for (int i = D->nbps - 1; i >= 0; i--) delete_breakpoint(D, i);
            free(D->bps);
            free(D);
            return;
        }
        buf[strcspn(buf, "\r\n")] = '\0';
        char *cmd = buf;
        while (*cmd == ' ') cmd++;
        char *arg = cmd + strcspn(cmd, " ");
        if (*arg) *arg++ = '\0';
        while (*arg == ' ') arg++;
        Frame *sel = nth_frame(I, D->selected);
        if (!*cmd) {
            continue;
        } else if (!strcmp(cmd, "c") || !strcmp(cmd, "continue")) {
            return;
        } else if (!strcmp(cmd, "n") || !strcmp(cmd, "next")) {
            D->step = STEP_OVER;
            D->step_depth = I->depth;
            return;
        } else if (!strcmp(cmd, "s") || !strcmp(cmd, "step")) {
            D->step = STEP_IN;
            return;
        } else if (!strcmp(cmd, "o") || !strcmp(cmd, "out")) {
            D->step = STEP_OUT;
            D->step_depth = I->depth;
            return;
        } else if (!strcmp(cmd, "p") || !strcmp(cmd, "print")) {
            Value v;
            char *err = eval_in(I, sel, arg, &v);
            if (err) {
                printf("error: %s\n", err);
                free(err);
            } else {
                show_value(I, NULL, v);
                release(v);
            }
        } else if (!strcmp(cmd, "locals")) {
            DebugVar vars[256];
            int n = frame_vars(sel, vars, 256);
            for (int i = 0; i < n; i++) {
                Value v = var_value(I, sel, &vars[i]);
                if (!IS_NIL(v)) show_value(I, vars[i].name->chars, v);
            }
        } else if (!strcmp(cmd, "globals")) {
            for (size_t i = 0; i < I->globals->used; i++) {
                MapEntry *e = &I->globals->entries[i];
                Value origin;
                if (!e->live || (map_get(I->constants, e->key, &origin) && (int)origin.as.number == ORIGIN_BUILTIN))
                    continue;
                if (e->value.type != VAL_LENT) show_value(I, lube_shown(AS_STRING(e->key)), e->value);
            }
        } else if (!strcmp(cmd, "bt") || !strcmp(cmd, "where")) {
            int i = 0;
            for (Frame *f = I->frame; f; f = f->prev, i++) {
                printf("%s#%d %s (%s:%d)\n", i == D->selected ? "=>" : "  ", i, f->name, f->src ? f->src->path : "?",
                       lube_frame_line(f));
            }
        } else if (!strcmp(cmd, "up") || !strcmp(cmd, "down") || !strcmp(cmd, "frame")) {
            int want = !strcmp(cmd, "up") ? D->selected + 1 : !strcmp(cmd, "down") ? D->selected - 1 : atoi(arg);
            if (want < 0 || want >= count_frames(I)) {
                printf("there is no frame %d (bt lists them)\n", want);
            } else {
                D->selected = want;
                show_frame(I, nth_frame(I, want), 0);
            }
        } else if (!strcmp(cmd, "l") || !strcmp(cmd, "list")) {
            list_source(sel);
        } else if (!strcmp(cmd, "b") || !strcmp(cmd, "break") || !strcmp(cmd, "tb") || !strcmp(cmd, "log")) {
            char file[512];
            int line;
            char *rest = arg + strcspn(arg, " ");
            if (*rest) *rest++ = '\0';
            while (*rest == ' ') rest++;
            if (!parse_location(I, arg, file, sizeof file, &line)) {
                printf("usage: b [file:]line [if condition]\n");
                continue;
            }
            Breakpoint *b = add_breakpoint(D, file, line);
            b->temporary = !strcmp(cmd, "tb");
            if (!strcmp(cmd, "log")) b->log = xstrndup(rest, strlen(rest));
            else if (!strncmp(rest, "if ", 3)) b->cond = xstrndup(rest + 3, strlen(rest + 3));
            else if (!strncmp(rest, "hits ", 5)) b->hit = xstrndup(rest + 5, strlen(rest + 5));
            printf("%s %d at %s:%d%s%s\n", b->log ? "logpoint" : "breakpoint", b->id, file, line, b->cond ? " if " : "",
                   b->cond ? b->cond : "");
        } else if (!strcmp(cmd, "bl")) {
            if (!D->nbps) printf("no breakpoints\n");
            for (int i = 0; i < D->nbps; i++) {
                Breakpoint *b = &D->bps[i];
                printf("%d %s:%d%s%s%s%s (hit %d time%s)\n", b->id, b->file, b->line, b->cond ? " if " : "",
                       b->cond ? b->cond : "", b->log ? " log " : "", b->log ? b->log : "", b->hits,
                       b->hits == 1 ? "" : "s");
            }
        } else if (!strcmp(cmd, "d") || !strcmp(cmd, "delete")) {
            int id = atoi(arg), found = 0;
            for (int i = D->nbps - 1; i >= 0; i--)
                if (!*arg || D->bps[i].id == id) {
                    delete_breakpoint(D, i);
                    found = 1;
                }
            if (!found) printf("there is no breakpoint %s (bl lists them)\n", arg);
        } else if (!strcmp(cmd, "catch")) {
            if (!strcmp(arg, "all")) D->catch_mode = CATCH_ALL;
            else if (!strcmp(arg, "uncaught")) D->catch_mode = CATCH_UNCAUGHT;
            else if (!strcmp(arg, "off")) D->catch_mode = CATCH_OFF;
            else printf("usage: catch all|uncaught|off\n");
        } else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit")) {
            fflush(stdout);
            exit(0);
        } else if (!strcmp(cmd, "h") || !strcmp(cmd, "help")) {
            fputs(HELP, stdout);
        } else {
            printf("unknown command '%s' (h lists them)\n", cmd);
        }
    }
}

/* lube --debug: stops at the first line, then takes commands. */
void lube_debug_terminal(Interp *I) {
    Debugger *D = xmalloc(sizeof(Debugger));
    memset(D, 0, sizeof *D);
    D->at_entry = 1;
    D->catch_mode = CATCH_UNCAUGHT;
    I->dbg = D;
    signal(SIGINT, on_interrupt);
    printf("Lube debugger: h lists the commands (Ctrl-C pauses the program)\n");
}

#else
/* LCOV_EXCL_START: never debugged (nothing starts the debugger), so never called */
void debug_hook(Interp *I) { (void)I; }
void debug_error(Interp *I) { (void)I; }
void debug_left_frame(Interp *I) { (void)I; }
/* LCOV_EXCL_STOP */
#endif
