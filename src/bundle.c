/*
 * bundle.c - building a Lube program into a standalone executable.
 *
 *   lube --build app.lube -o app
 *
 * copies the interpreter itself and appends the program: app.lube and every
 * file it imports (found by checking each one, so a program with errors does
 * not build). The result runs on its own, without Lube installed:
 *
 *   [the lube executable] [payload] [trailer]
 *   payload:  u32 file count, then for each file
 *             u32 path length, path, u32 text length, text
 *   trailer:  u64 payload length, then the 8 bytes "LUBEPACK"
 *
 * Numbers are little-endian. The first file is the program; paths are
 * relative to its folder, as imports compute them. At startup, lube looks
 * for the trailer at the end of its own file and, when it is there, runs the
 * packed program with every command-line argument in `args`.
 */
#if defined(__unix__) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* chmod under -std=c99 */
#endif
#include "lube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>  /* chmod: the executable bit */
#endif

static const char MAGIC[8] = {'L', 'U', 'B', 'E', 'P', 'A', 'C', 'K'};
#define TRAILER 16

static void put_u32(StrBuf *sb, size_t n) {
    for (int i = 0; i < 4; i++) sb_putc(sb, (char)((n >> (8 * i)) & 0xff));
}

static size_t get_uint(const unsigned char *p, int bytes) {
    size_t n = 0;
    for (int i = bytes - 1; i >= 0; i--) n = (n << 8) | p[i];
    return n;
}

/* The path of the running executable: Linux's /proc/self/exe, or argv[0]
 * (as given, or found on PATH); Windows says where it is. NULL when it
 * cannot be found. */
static char *find_self(const char *argv0) {
#ifdef _WIN32
    (void)argv0;
    return os_self_path();
#else
    FILE *f = fopen("/proc/self/exe", "rb");
    if (f) {
        fclose(f);                              /* LCOV_EXCL_START: Linux only */
        return xstrndup("/proc/self/exe", 14);
    }                                           /* LCOV_EXCL_STOP */
    if (strchr(argv0, '/')) return xstrndup(argv0, strlen(argv0));
    const char *dirs = getenv("PATH");
    StrBuf path;
    sb_init(&path);
    while (dirs && *dirs) {
        const char *end = strchr(dirs, ':');
        size_t n = end ? (size_t)(end - dirs) : strlen(dirs);
        path.len = 0;
        sb_putn(&path, n ? dirs : ".", n ? n : 1);
        sb_putc(&path, '/');
        sb_puts(&path, argv0);
        if ((f = fopen(path.data, "rb"))) {
            fclose(f);
            return path.data;
        }
        dirs = end ? end + 1 : NULL;
    }
    sb_free(&path);
    return NULL;
#endif
}

/* ---------- building ---------- */

typedef struct {
    char **keys;     /* each file's name in the bundle, as the program's imports compute it */
    char **texts;
    size_t *lens;
    int count, cap;
    const char *dir; /* the program's folder, where relative names are read from */
    size_t dirlen;
} Files;

static int add_file(Interp *I, Files *fs, const char *key, const char *real, const char *importer, const char *rel);

/* Adds every file of package pkg (imported by `importer`), named in the
 * bundle by the package's folder: a/b/file.lube, as it is looked up. */
static int add_package(Interp *I, Files *fs, const char *pkg, const char *importer) {
    StrBuf folder, key, real;
    sb_init(&folder);
    ObjString *name = lube_intern(I, pkg, strlen(pkg));
    lube_find_package(I, name, &folder);  /* found when the import was checked */
    int n, status = 0;
    char **files = lube_package_files(I, folder.data, &n);
    for (int i = 0; i < n; i++) {
        sb_init(&key);
        sb_init(&real);
        for (const char *p = pkg; *p; p++) sb_putc(&key, *p == '.' ? '/' : *p);
        sb_printf(&key, "/%s", files[i]);
        sb_printf(&real, "%s/%s", folder.data, files[i]);
        if (!status) status = add_file(I, fs, key.data, real.data, importer, pkg);
        sb_free(&key);
        sb_free(&real);
        free(files[i]);
    }
    free(files);
    sb_free(&folder);
    return status;
}

/* Reads and checks the file named `key` (read from `real`, or from the
 * program's folder), then adds it and the modules and packages it imports.
 * (importer, rel): who imported it, for the message if missing. */
static int add_file(Interp *I, Files *fs, const char *key, const char *real, const char *importer, const char *rel) {
    for (int i = 0; i < fs->count; i++)
        if (!strcmp(fs->keys[i], key)) return 0;  /* already packed */
    StrBuf path;
    sb_init(&path);
    if (real) {
        sb_puts(&path, real);
    } else {
        if (key[0] != '/') sb_putn(&path, fs->dir, fs->dirlen);
        sb_puts(&path, key);
    }
    size_t len;
    char *text = read_file(path.data, &len);
    if (!text) {
        if (importer) fprintf(stderr, "lube: cannot build: '%s' (imported by %s) not found\n", rel, importer);
        else fprintf(stderr, "lube: cannot open file '%s'\n", path.data);
        sb_free(&path);
        return 66;
    }
    if (fs->count == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 8;
        fs->keys = xrealloc(fs->keys, sizeof(char *) * (size_t)fs->cap);
        fs->texts = xrealloc(fs->texts, sizeof(char *) * (size_t)fs->cap);
        fs->lens = xrealloc(fs->lens, sizeof(size_t) * (size_t)fs->cap);
    }
    int me = fs->count++;
    fs->keys[me] = xstrndup(key, strlen(key));
    fs->texts[me] = text;
    fs->lens[me] = len;

    ObjList *found = new_list();
    I->imports_found = found;
    int status = lube_check_source(I, lube_add_source(I, path.data, xstrndup(text, len), len));
    I->imports_found = NULL;
    for (size_t i = 0; i < found->count && status == 0; i++) {
        const char *what = AS_STRING(found->items[i])->chars;
        if (what[0] == '\x01') {  /* a package */
            status = add_package(I, fs, what + 1, path.data);
            continue;
        }
        StrBuf sub;
        sb_init(&sub);
        lube_module_path(&sub, fs->keys[me], what);
        status = add_file(I, fs, sub.data, NULL, path.data, what);
        sb_free(&sub);
    }
    release(OBJ_VAL(found));
    sb_free(&path);
    return status;
}

/* lube --build: the program, packed after a copy of this lube; native: after
 * a lube compiled for it, with machine code for the functions that can have
 * it (native.c). */
int lube_build(Interp *I, const char *argv0, const char *main_path, const char *out_path, int native) {
    Files fs = {0};
    const char *slash = strrchr(main_path, '/');
    fs.dir = main_path;
    fs.dirlen = slash ? (size_t)(slash - main_path + 1) : 0;
    if (!I->root) I->root = slash ? xstrndup(main_path, (size_t)(slash - main_path)) : xstrndup(".", 1);
    int status = add_file(I, &fs, main_path + fs.dirlen, NULL, NULL, NULL);
    StrBuf compiled;  /* native: the lube made for this program */
    sb_init(&compiled);
    sb_printf(&compiled, "%s.native-tmp", out_path);
#ifdef _WIN32
    sb_puts(&compiled, ".exe");
#endif
    if (native && !status) {
        status = lube_native_build(I, fs.count, fs.keys, fs.texts, fs.lens, argv0, compiled.data, native == 2);
        if (status < 0) native = status = 0;  /* nothing could be native: this lube as it is */
    }
    char *self = status ? NULL : native ? xstrndup(compiled.data, compiled.len) : find_self(argv0);
    size_t exe_len = 0;
    char *exe = self ? read_file(self, &exe_len) : NULL;
    if (!status && !exe) {
        fprintf(stderr, "lube: cannot build: cannot read the lube executable itself (run lube with its full path)\n");
        status = 70;
    }
    if (!status) {
        StrBuf payload;
        sb_init(&payload);
        put_u32(&payload, (size_t)fs.count);
        for (int i = 0; i < fs.count; i++) {
            put_u32(&payload, strlen(fs.keys[i]));
            sb_puts(&payload, fs.keys[i]);
            put_u32(&payload, fs.lens[i]);
            sb_putn(&payload, fs.texts[i], fs.lens[i]);
        }
        size_t size = payload.len;  /* (a built program never builds, so exe is plain lube) */
        for (int i = 0; i < 8; i++) sb_putc(&payload, (char)((size >> (8 * i)) & 0xff));
        sb_putn(&payload, MAGIC, 8);

        FILE *out = fopen(out_path, "wb");
        int ok = out && fwrite(exe, 1, exe_len, out) == exe_len && fwrite(payload.data, 1, payload.len, out) == payload.len;
        if (out && fclose(out)) ok = 0;
        if (ok) {
#if defined(__unix__) || defined(__APPLE__)
            chmod(out_path, 0755);
#endif
            printf("built %s: %d file%s, %lu bytes\n", out_path, fs.count, fs.count == 1 ? "" : "s",
                   (unsigned long)(exe_len + payload.len));
        } else {
            fprintf(stderr, "lube: cannot write '%s'\n", out_path);
            status = 73;
        }
        sb_free(&payload);
    }
    for (int i = 0; i < fs.count; i++) {
        free(fs.keys[i]);
        free(fs.texts[i]);
    }
    free(fs.keys);
    free(fs.texts);
    free(fs.lens);
    if (native) remove(compiled.data);
    sb_free(&compiled);
    free(self);
    free(exe);
    return status;
}

/* ---------- running a built program ---------- */

/* Reads one length-prefixed piece of the payload at *p, if it fits. */
static int take(const unsigned char **p, const unsigned char *end, const char **s, size_t *len) {
    if (end - *p < 4) return 0;
    size_t n = get_uint(*p, 4);
    if (n > (size_t)(end - *p - 4)) return 0;
    *s = (const char *)*p + 4;
    *len = n;
    *p += 4 + n;
    return 1;
}

/* When this executable carries a program, runs it and returns its exit
 * status; otherwise returns -1. */
int lube_run_bundle(Interp *I, const char *argv0, int argc, char **argv) {
    char *self = find_self(argv0);
    FILE *f = self ? fopen(self, "rb") : NULL;
    free(self);
    if (!f) return -1;
    unsigned char trailer[TRAILER];
    unsigned char *payload = NULL;
    size_t size = 0;
    if (!fseek(f, -TRAILER, SEEK_END) && fread(trailer, 1, TRAILER, f) == TRAILER && !memcmp(trailer + 8, MAGIC, 8)) {
        size = get_uint(trailer, 8);
        payload = size <= lube_max_bytes ? malloc(size ? size : 1) : NULL;
        if (payload && (fseek(f, -(long)(TRAILER + size), SEEK_END) || fread(payload, 1, size, f) != size)) {
            free(payload);  /* LCOV_EXCL_START: a damaged executable */
            payload = NULL;
        }                   /* LCOV_EXCL_STOP */
    }
    fclose(f);
    if (!payload) return -1;

    /* unpack: the files go into I->bundle, the first one is the program */
    I->bundle = new_map();
    const unsigned char *p = payload + (size >= 4 ? 4 : size), *end = payload + size;
    size_t count = size >= 4 ? get_uint(payload, 4) : 0;
    Value first = NIL_VAL;
    for (size_t i = 0; i < count; i++) {
        const char *path, *text;
        size_t plen, tlen;
        if (!take(&p, end, &path, &plen) || !take(&p, end, &text, &tlen)) break;
        Value key = OBJ_VAL(new_string(path, plen));
        if (i == 0) first = retain(key);
        map_set(I->bundle, key, OBJ_VAL(new_string(text, tlen)));
    }
    free(payload);
    if (IS_NIL(first)) {  /* LCOV_EXCL_START: a damaged executable */
        fprintf(stderr, "lube: this executable's program is damaged\n");
        return 70;
    }                     /* LCOV_EXCL_STOP */

    ObjList *args = new_list();
    for (int i = 1; i < argc; i++) list_push(args, OBJ_VAL(new_string(argv[i], strlen(argv[i]))));
    lube_define(I, "args", OBJ_VAL(args));
    Value text;
    map_get(I->bundle, first, &text);
    ObjString *t = AS_STRING(text);
    Source *src = lube_add_source(I, AS_STRING(first)->chars, xstrndup(t->chars, t->length), t->length);
    release(first);
    return lube_run_source(I, src, 0);
}
