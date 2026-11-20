/*
 * fs.c - the plugin fs: files and folders.
 *
 *   import fs.*                       (or import fs: fs.open(...))
 *   let f = open("save.bin", "w")     a File: f.write(data), f.read(n), f.read_line(),
 *   f.write(bytes)                    f.seek(pos, from), f.tell(), f.size(), f.eof(),
 *   f.close()                         f.flush(), f.close()
 *   exists, is_dir, list_dir, make_dir, remove_file, rename_file, file_size,
 *   read_bytes, write_bytes
 *
 * Files are always read and written as bytes. Copies of a File share one
 * open file, which closes with the last of them (or at close()).
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* mkdir, fseeko under -std=c99 */
#endif
#define _FILE_OFFSET_BITS 64     /* files past 2 GB on 32-bit systems (a Raspberry Pi's) */
#include "../lube_plugin.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef __int64 FilePos;  /* positions in a file, and its size: 64-bit everywhere */
#define file_seek _fseeki64
#define file_tell _ftelli64
typedef struct _stat64 StatBuf;
#define stat_call _stat64
#ifndef S_ISDIR  /* (hidden by some headers under -std=c99) */
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <dirent.h>
#include <unistd.h>
typedef off_t FilePos;
#define file_seek fseeko
#define file_tell ftello
typedef struct stat StatBuf;
#define stat_call stat
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "f_open" -> "open" */
#define NEED(cond, i, what)                                                                                    \
    do {                                                                                                       \
        if (!(cond))                                                                                           \
            return lube->error(vm, "%s() expects %s as argument %d, got %s", FNAME, (what), (i) + 1,          \
                               lube->type_name(argv[i]));                                                      \
    } while (0)
#define NEED_STR(i) NEED(lube->kind(argv[i]) == LUBE_STR, i, "a str")
#define NEED_NUM(i) NEED(lube->kind(argv[i]) == LUBE_NUMBER, i, "a Number")
#define STR(i) lube->str(argv[i], NULL)

static const LubeAPI *lube;
static LubeType *FileType;

typedef struct {
    FILE *f;    /* NULL once closed */
    char *path;
} File;

static LubeValue io_error(LubeVM *vm, const char *what, const char *path) {
    return lube->error(vm, "%s '%s': %s", what, path, strerror(errno));
}

static char *copy_of(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = (char *)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

/* A growing buffer of bytes (malloc'd). */
typedef struct {
    unsigned char *data;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        unsigned char *d = (unsigned char *)realloc(b->data, cap);
        if (!d) abort();  /* LCOV_EXCL_LINE: out of memory */
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* ======================================================================
 * File
 * ==================================================================== */

static void file_release(void *data) {
    File *x = (File *)data;
    if (x->f) fclose(x->f);
    free(x->path);
}

static int file_describe(const void *data, char *buf, size_t size) {
    const File *x = (const File *)data;
    return snprintf(buf, size, "File(%s%s)", x->path, x->f ? "" : ", closed");
}

/* open(path, mode = "r"): r (read), w (write, emptied first), a (add at the
 * end), r+ / w+ / a+ (both); always as bytes. */
FN(f_open) {
    NEED_STR(0);
    const char *mode = "r";
    if (argc > 1) {
        NEED_STR(1);
        mode = STR(1);
    }
    static const char *const modes[][2] = {{"r", "rb"}, {"w", "wb"}, {"a", "ab"}, {"r+", "r+b"}, {"w+", "w+b"}, {"a+", "a+b"}};
    const char *how = NULL;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (!strcmp(mode, modes[i][0])) how = modes[i][1];
    if (!how) return lube->error(vm, "open(): the mode must be \"r\", \"w\", \"a\", \"r+\", \"w+\" or \"a+\", not \"%s\"", mode);
    FILE *fp = fopen(STR(0), how);
    if (!fp) return io_error(vm, "cannot open", STR(0));
    void *data;
    LubeValue f = lube->new_foreign(vm, FileType, &data);
    ((File *)data)->f = fp;
    ((File *)data)->path = copy_of(STR(0));
    return f;
}

/* The File of argument 0 (a method's), its FILE open: or NULL, with *err the error. */
static File *open_file(LubeVM *vm, LubeValue v, const char *fn, LubeValue *err) {
    File *x = (File *)lube->foreign(v, FileType);
    if (x->f) return x;
    *err = lube->error(vm, "%s(): the file '%s' is closed", fn, x->path);
    return NULL;
}

/* f.read(n): up to n bytes (fewer at the end); f.read(): the rest. */
FN(m_read) {
    LubeValue err;
    File *x = open_file(vm, argv[0], "read", &err);
    if (!x) return err;
    double want = -1;
    if (argc > 1) {
        want = lube->kind(argv[1]) == LUBE_NUMBER ? lube->number(argv[1]) : -1;
        if (want != floor(want) || want < 0) return lube->error(vm, "read(n): n must be a whole Number, 0 or more");
    }
    Buf b = {0};
    unsigned char chunk[65536];
    for (;;) {
        size_t ask = sizeof chunk;
        if (want >= 0 && (double)(b.len + ask) > want) ask = (size_t)(want - (double)b.len);
        if (ask == 0) break;
        size_t got = fread(chunk, 1, ask, x->f);
        if (got) buf_add(&b, chunk, got);
        if (got < ask) break;
    }
    LubeValue out = lube->new_bytes(vm, b.data, b.len);
    free(b.data);
    return out;
}

/* f.read_line(): the next line, without its end ("\n" or "\r\n"); an error at the end. */
FN(m_read_line) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "read_line", &err);
    if (!x) return err;
    Buf b = {0};
    int c, got = 0;
    while ((c = fgetc(x->f)) != EOF) {
        got = 1;
        if (c == '\n') break;
        unsigned char ch = (unsigned char)c;
        buf_add(&b, &ch, 1);
    }
    if (!got) return lube->error(vm, "read_line(): end of the file '%s'", x->path);
    if (b.len && b.data[b.len - 1] == '\r') b.len--;
    LubeValue s = lube->new_str(vm, b.data ? (const char *)b.data : "", b.len);
    free(b.data);
    return s;
}

/* f.write(x): x's bytes (Bytes, or a str's); returns how many. */
FN(m_write) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "write", &err);
    if (!x) return err;
    const void *p;
    size_t n;
    if (lube->kind(argv[1]) == LUBE_BYTES) p = lube->bytes(argv[1], &n);
    else if (lube->kind(argv[1]) == LUBE_STR) p = lube->str(argv[1], &n);
    else return lube->error(vm, "write() expects Bytes or a str as argument 2, got %s", lube->type_name(argv[1]));
    if (n && fwrite(p, 1, n, x->f) != n) return io_error(vm, "cannot write to", x->path);  /* LCOV_EXCL_LINE: a full disk */
    return lube->number_value((double)n);
}

/* f.seek(pos, from = "start"): from "start", "current" or "end". */
FN(m_seek) {
    LubeValue err;
    File *x = open_file(vm, argv[0], "seek", &err);
    if (!x) return err;
    NEED_NUM(1);
    int whence = SEEK_SET;
    if (argc > 2) {
        NEED_STR(2);
        const char *w = STR(2);
        whence = !strcmp(w, "start") ? SEEK_SET : !strcmp(w, "current") ? SEEK_CUR : !strcmp(w, "end") ? SEEK_END : -1;
        if (whence < 0) return lube->error(vm, "seek(): from must be \"start\", \"current\" or \"end\", not \"%s\"", w);
    }
    double pos = lube->number(argv[1]);
    if (pos != floor(pos) || fabs(pos) > 9e15) return lube->error(vm, "seek(): the position must be a whole Number");
    if (file_seek(x->f, (FilePos)pos, whence)) return io_error(vm, "cannot seek in", x->path);
    return lube->nothing();
}

FN(m_tell) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "tell", &err);
    return x ? lube->number_value((double)file_tell(x->f)) : err;
}

/* f.size(): its length in bytes (where it is reading stays). */
FN(m_size) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "size", &err);
    if (!x) return err;
    FilePos at = file_tell(x->f);
    file_seek(x->f, 0, SEEK_END);
    FilePos size = file_tell(x->f);
    file_seek(x->f, at, SEEK_SET);
    return lube->number_value((double)size);
}

FN(m_flush) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "flush", &err);
    if (!x) return err;
    fflush(x->f);
    return lube->nothing();
}

/* f.eof(): at the end (nothing more to read). */
FN(m_eof) {
    (void)argc;
    LubeValue err;
    File *x = open_file(vm, argv[0], "eof", &err);
    if (!x) return err;
    int c = fgetc(x->f);
    if (c == EOF) return lube->boolean(1);
    ungetc(c, x->f);
    return lube->boolean(0);
}

/* f.close(): closes it (closing it again does nothing). */
FN(m_close) {
    (void)vm;
    (void)argc;
    File *x = (File *)lube->foreign(argv[0], FileType);
    if (x->f) fclose(x->f);
    x->f = NULL;
    return lube->nothing();
}

/* ======================================================================
 * Folders
 * ==================================================================== */

static int stat_of(const char *path, StatBuf *st) { return stat_call(path, st) == 0; }

FN(f_exists) {
    (void)argc;
    NEED_STR(0);
    StatBuf st;
    return lube->boolean(stat_of(STR(0), &st));
}

FN(f_is_dir) {
    (void)argc;
    NEED_STR(0);
    StatBuf st;
    return lube->boolean(stat_of(STR(0), &st) && S_ISDIR(st.st_mode));
}

FN(f_file_size) {
    (void)argc;
    NEED_STR(0);
    StatBuf st;
    if (!stat_of(STR(0), &st)) return io_error(vm, "cannot find", STR(0));
    return lube->number_value((double)st.st_size);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The names in a folder but . and .. (malloc'd), sorted; *n -1 if it is not one. */
static char **names_in(const char *dir, int *n) {
    char **names = NULL;
    int cap = 0;
    *n = -1;
#ifdef _WIN32
    char pattern[4096];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    *n = 0;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *e;
    *n = 0;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
#endif
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            char **more = (char **)realloc(names, sizeof(char *) * (size_t)cap);
            if (!more) abort();  /* LCOV_EXCL_LINE: out of memory */
            names = more;
        }
        names[(*n)++] = copy_of(name);
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    if (*n) qsort(names, (size_t)*n, sizeof(char *), by_name);
    return names;
}

/* list_dir(path): the names in a folder (files and folders), sorted. */
FN(f_list_dir) {
    (void)argc;
    NEED_STR(0);
    int n;
    char **names = names_in(STR(0), &n);
    if (n < 0) return lube->error(vm, "list_dir(): '%s' is not a folder", STR(0));
    LubeValue l = lube->new_list(vm);
    for (int i = 0; i < n; i++) {
        lube->list_push(l, lube->new_str(vm, names[i], strlen(names[i])));
        free(names[i]);
    }
    free(names);
    return l;
}

static int make_one(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

/* make_dir(path): makes the folder, and the folders it is in that are not there yet. */
FN(f_make_dir) {
    (void)argc;
    NEED_STR(0);
    size_t len;
    const char *given = lube->str(argv[0], &len);
    char *path = copy_of(given);
    StatBuf st;
    for (size_t i = 1; i <= len; i++) {
        if (i < len && path[i] != '/' && path[i] != '\\') continue;
        char c = path[i];
        path[i] = '\0';
        int made = stat_of(path, &st) ? S_ISDIR(st.st_mode) : make_one(path) == 0;
        if (!made && stat_of(path, &st)) errno = ENOTDIR;  /* (a file is in the way) */
        path[i] = c;
        if (!made) {
            free(path);
            return io_error(vm, "cannot make the folder", given);
        }
    }
    free(path);
    return lube->nothing();
}

/* remove_file(path): removes a file, or an empty folder. */
FN(f_remove_file) {
    (void)argc;
    NEED_STR(0);
    const char *path = STR(0);
    StatBuf st;
    int dir = stat_of(path, &st) && S_ISDIR(st.st_mode);
#ifdef _WIN32
    int bad = dir ? _rmdir(path) : remove(path);
#else
    int bad = dir ? rmdir(path) : remove(path);
#endif
    return bad ? io_error(vm, "cannot remove", path) : lube->nothing();
}

/* rename_file(from, to): renames (or moves) it, replacing `to` if it is there. */
FN(f_rename_file) {
    (void)argc;
    NEED_STR(0);
    NEED_STR(1);
    const char *from = STR(0), *to = STR(1);
#ifdef _WIN32
    if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
              : e == ERROR_ALREADY_EXISTS ? EEXIST : e == ERROR_NOT_SAME_DEVICE ? EXDEV : EACCES;
        return io_error(vm, "cannot rename", from);
    }
    return lube->nothing();
#else
    return rename(from, to) ? io_error(vm, "cannot rename", from) : lube->nothing();
#endif
}

/* read_bytes(path): the whole file. */
FN(f_read_bytes) {
    (void)argc;
    NEED_STR(0);
    FILE *fp = fopen(STR(0), "rb");
    if (!fp) return io_error(vm, "cannot open", STR(0));
    Buf b = {0};
    unsigned char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) buf_add(&b, chunk, n);
    fclose(fp);
    LubeValue out = lube->new_bytes(vm, b.data, b.len);
    free(b.data);
    return out;
}

/* write_bytes(path, bytes): the file becomes these bytes. */
FN(f_write_bytes) {
    (void)argc;
    NEED_STR(0);
    NEED(lube->kind(argv[1]) == LUBE_BYTES, 1, "Bytes");
    FILE *fp = fopen(STR(0), "wb");
    if (!fp) return io_error(vm, "cannot open", STR(0));
    size_t n;
    const unsigned char *p = lube->bytes(argv[1], &n);
    int wrote = !n || fwrite(p, 1, n, fp) == n;
    int ok = (fclose(fp) == 0) && wrote;
    return ok ? lube->nothing() : io_error(vm, "cannot write to", STR(0));  /* LCOV_EXCL_LINE: a full disk */
}

LUBE_PLUGIN(fs) {
    lube = api;
    api->function(plugin, "open", f_open, 1, 2);
    api->function(plugin, "exists", f_exists, 1, 1);
    api->function(plugin, "is_dir", f_is_dir, 1, 1);
    api->function(plugin, "file_size", f_file_size, 1, 1);
    api->function(plugin, "list_dir", f_list_dir, 1, 1);
    api->function(plugin, "make_dir", f_make_dir, 1, 1);
    api->function(plugin, "remove_file", f_remove_file, 1, 1);
    api->function(plugin, "rename_file", f_rename_file, 2, 2);
    api->function(plugin, "read_bytes", f_read_bytes, 1, 1);
    api->function(plugin, "write_bytes", f_write_bytes, 2, 2);
    FileType = api->type(plugin, "File", sizeof(File), file_release, file_describe);
    api->method(FileType, "read", m_read, 1, 2);
    api->method(FileType, "read_line", m_read_line, 1, 1);
    api->method(FileType, "write", m_write, 2, 2);
    api->method(FileType, "seek", m_seek, 2, 3);
    api->method(FileType, "tell", m_tell, 1, 1);
    api->method(FileType, "size", m_size, 1, 1);
    api->method(FileType, "flush", m_flush, 1, 1);
    api->method(FileType, "eof", m_eof, 1, 1);
    api->method(FileType, "close", m_close, 1, 1);
    return LUBE_PLUGIN_API;
}
