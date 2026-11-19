/*
 * util.c - small helpers: growable string buffers, checked allocation,
 * reading files, and printing diagnostics that point at the source line.
 */
#include "lube.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Running out of memory stops the program cleanly instead of continuing
 * with a failed allocation. */
void lube_out_of_memory(void) {
    fflush(stdout);
    fputs("lube: out of memory\n", stderr);
    exit(70);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) lube_out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) lube_out_of_memory();
    return q;
}

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ---------- arena: many small allocations, freed all at once ---------- */

typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
} Chunk;

struct Arena {
    Chunk *head;
};

#define ALIGN_UP(n) (((n) + 15) & ~(size_t)15)
#define CHUNK_HEADER ALIGN_UP(sizeof(Chunk))

Arena *arena_new(void) {
    Arena *a = xmalloc(sizeof(Arena));
    a->head = NULL;
    return a;
}

/* Returns n zeroed bytes that live until arena_free(). */
void *arena_alloc(Arena *a, size_t n) {
    if (n > LUBE_MAX_BYTES) lube_out_of_memory();
    n = ALIGN_UP(n ? n : 1);
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 16384 ? n : 16384;
        Chunk *c = xmalloc(CHUNK_HEADER + cap);
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    char *p = (char *)a->head + CHUNK_HEADER + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}

void *arena_grow(Arena *a, void *old, size_t old_size, size_t new_size) {
    void *p = arena_alloc(a, new_size);
    if (old_size) memcpy(p, old, old_size);
    return p;
}

void arena_free(Arena *a) {
    if (!a) return;
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

/* ---------- string buffer ---------- */

void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (extra > LUBE_MAX_BYTES - sb->len) lube_out_of_memory();
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) cap *= 2;
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_putn(StrBuf *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (n) memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_putn(sb, s, strlen(s)); }

void sb_putc(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb_init(sb);
}

/* ---------- files ---------- */

char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    StrBuf sb;
    sb_init(&sb);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_putn(&sb, buf, n);
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        sb_free(&sb);
        return NULL;
    }
    if (!sb.data) sb_putn(&sb, "", 0);
    if (length) *length = sb.len;
    return sb.data;
}

/* ---------- diagnostics ---------- */

static void std_output(const char *text, size_t len, int is_err) {
    if (is_err) fflush(stdout);  /* messages come after the output before them */
    fwrite(text, 1, len, is_err ? stderr : stdout);
}

/* Where a program's output and Lube's messages go: stdout and stderr, or
 * (under lube --dap, whose stdout carries the protocol) somewhere else. Set
 * once, before any isolate starts. */
void (*lube_output)(const char *text, size_t len, int is_err) = std_output;

/* Prints a message (err: an error message, for stderr). */
void lube_print(int err, const char *fmt, ...) {
    StrBuf sb;
    sb_init(&sb);
    va_list ap;
    va_start(ap, fmt);
    char small[512];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof small) {  /* long: format again into the right size */
        sb_reserve(&sb, (size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(sb.data, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sb.len = (size_t)n;
    } else {
        sb_putn(&sb, small, (size_t)(n < 0 ? 0 : n));
    }
    lube_output(sb.data ? sb.data : "", sb.len, err);
    sb_free(&sb);
}

/* Prints the source line `line` of `src`, with a caret under column `col`
 * (no caret when col <= 0). */
void print_source_line(Source *src, int line, int col) {
    if (!src || !src->text || line < 1) return;
    const char *p = src->text, *end = src->text + src->length;
    int cur = 1;
    while (p < end && cur < line) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return;
    const char *e = p;
    while (e < end && *e != '\n') e++;
    if (e > p && e[-1] == '\r') e--;
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%5d | %.*s\n", line, (int)(e - p), p);
    if (col > 0) {
        sb_puts(&sb, "      | ");
        for (int i = 0; i < col - 1 && p + i < e; i++) sb_putc(&sb, p[i] == '\t' ? '\t' : ' ');
        sb_puts(&sb, "^\n");
    }
    lube_print(1, "%s", sb.data);
    sb_free(&sb);
}

void report_error(Source *src, int line, int col, const char *kind, const char *msg) {
    fflush(stdout);
    const char *path = src ? src->path : "<input>";
    if (col > 0) lube_print(1, "%s:%d:%d: %s: %s\n", path, line, col, kind, msg);
    else lube_print(1, "%s:%d: %s: %s\n", path, line, kind, msg);
    print_source_line(src, line, col);
}
