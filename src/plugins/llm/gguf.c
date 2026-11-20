/*
 * llm/gguf.c - GGUF files (llama.cpp's): a header, metadata (keys and
 * values), the tensors' names, shapes, types and offsets, then their data,
 * aligned. The file is mapped into memory (or read into it once), and
 * everything here points into it. (Part of llm.c.)
 */

/* the types of metadata values */
enum { GV_U8, GV_I8, GV_U16, GV_I16, GV_U32, GV_I32, GV_F32, GV_BOOL, GV_STR, GV_ARR, GV_U64, GV_I64, GV_F64,
       GV_NTYPES };
static const int GV_SIZE[GV_NTYPES] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

typedef struct {
    const char *s;  /* (not NUL-terminated: it points into the file) */
    size_t n;
} Str;

typedef struct {
    Str key;
    int type;                   /* GV_... */
    int etype;                  /* an array's elements' type */
    uint64_t count;             /* an array's length */
    const unsigned char *at;    /* the value, or an array's first element */
} Kv;

typedef struct {
    Str name;
    int type;                   /* ggml's: T_F32... (quant.c) */
    int ndim;
    int64_t ne[4];              /* ne[0] is the length of a row, ne[1] the number of rows */
    const unsigned char *data;
    uint64_t nbytes;
} Tensor;

enum { HELD_NONE, HELD_MAPPED, HELD_READ };

typedef struct {
    unsigned char *base;        /* the whole file */
    size_t size;
    int held;                   /* HELD_MAPPED: a mapping of the file; HELD_READ: malloc'd */
    uint32_t version;
    Kv *kv;
    int64_t n_kv;
    Tensor *tensors;
    int64_t n_tensors;
} Gguf;

static int str_is(Str s, const char *c) { return s.n == strlen(c) && !memcmp(s.s, c, s.n); }

/* little-endian numbers, as GGUF has them */
static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd_u64(const unsigned char *p) { return (uint64_t)rd_u32(p) | (uint64_t)rd_u32(p + 4) << 32; }

/* ---------- the file in memory ---------- */

static void gguf_free(Gguf *g) {
    if (g->held == HELD_MAPPED) {
#ifdef _WIN32
        UnmapViewOfFile(g->base);
#else
        munmap(g->base, g->size);
#endif
    } else if (g->held == HELD_READ) {
        free(g->base);
    }
    free(g->kv);
    free(g->tensors);
    memset(g, 0, sizeof *g);
}

/* Whether n bytes can be in memory at once (a 32-bit system's size_t is short) */
static int fits_memory(uint64_t n) { return n == (uint64_t)(size_t)n; }

/* Maps the file into memory: 1, or 0 if it cannot (it is then read). */
static int map_file(Gguf *g, const char *path) {
#ifdef _WIN32
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    HANDLE m = NULL;
    void *p = NULL;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && fits_memory((uint64_t)size.QuadPart))
        m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (m) p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (m) CloseHandle(m);  /* (the view keeps the file open) */
    CloseHandle(f);
    if (!p) return 0;
    g->size = (size_t)size.QuadPart;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    void *p = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && fits_memory((uint64_t)st.st_size))
        p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 0;
    g->size = (size_t)st.st_size;
#endif
    g->base = (unsigned char *)p;
    g->held = HELD_MAPPED;
    return 1;
}

/* Reads the whole file into memory: 0 (with errno) if it cannot. */
static int read_file(Gguf *g, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    file_seek(f, 0, SEEK_END);
    FilePos size = file_tell(f);
    file_seek(f, 0, SEEK_SET);
    g->base = fits_memory((uint64_t)(size > 0 ? size : 0)) ? (unsigned char *)malloc(size > 0 ? (size_t)size : 1) : NULL;
    if (!g->base) {  /* LCOV_EXCL_START: out of memory (or room) */
        fclose(f);
        errno = ENOMEM;
        return 0;
    }  /* LCOV_EXCL_STOP */
    g->size = size > 0 ? (size_t)size : 0;
    g->held = HELD_READ;
    size_t got = fread(g->base, 1, g->size, f);
    fclose(f);
    if (got != g->size) {  /* LCOV_EXCL_START: a read error after opening it */
        errno = EIO;
        return 0;
    }  /* LCOV_EXCL_STOP */
    return 1;
}

/* ---------- reading it ---------- */

typedef struct {
    const unsigned char *p, *end;
    int bad;
} Rd;

static uint64_t rd_n(Rd *r, int n) {
    if (r->bad || r->end - r->p < n) {
        r->bad = 1;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)r->p[i] << (8 * i);
    r->p += n;
    return v;
}

static Str rd_str(Rd *r) {
    Str s = {"", 0};
    uint64_t n = rd_n(r, 8);
    if (r->bad || n > (uint64_t)(r->end - r->p)) {
        r->bad = 1;
        return s;
    }
    s.s = (const char *)r->p;
    s.n = (size_t)n;
    r->p += n;
    return s;
}

/* Skips a value of type t, one of GV_... (arrays may hold arrays, a few deep). */
static void rd_skip(Rd *r, uint32_t t, int depth) {
    if (t == GV_STR) {
        rd_str(r);
    } else if (t == GV_ARR) {
        uint32_t et = (uint32_t)rd_n(r, 4);
        uint64_t n = rd_n(r, 8);
        if (et >= GV_NTYPES || depth > 3) r->bad = 1;
        if (r->bad) return;
        if (GV_SIZE[et]) {
            if (n > (uint64_t)(r->end - r->p) / (uint64_t)GV_SIZE[et]) r->bad = 1;
            else r->p += n * (uint64_t)GV_SIZE[et];
        } else {
            for (uint64_t i = 0; i < n && !r->bad; i++) rd_skip(r, et, depth + 1);
        }
    } else {  /* (a number: the types were checked) */
        rd_n(r, GV_SIZE[t]);
    }
}

static int type_known(uint32_t t);
static int64_t type_row_bytes(int t, int64_t n);
static const char *type_name(int t);
static int type_block(int t);

/* Reads g->base: 1, or 0 with a message in err. */
static int gguf_parse(Gguf *g, char *err, size_t errsize) {
    Rd r = {g->base, g->base + g->size, 0};
    if (g->size < 24 || memcmp(g->base, "GGUF", 4)) {
        snprintf(err, errsize, "is not a GGUF file");
        return 0;
    }
    r.p += 4;
    g->version = (uint32_t)rd_n(&r, 4);
    if (g->version != 2 && g->version != 3) {
        snprintf(err, errsize, "is a GGUF file of version %u (this reads versions 2 and 3)", (unsigned)g->version);
        return 0;
    }
    uint64_t n_tensors = rd_n(&r, 8), n_kv = rd_n(&r, 8);
    /* (each takes a dozen bytes at least: a count past that is a damaged file, not one to allocate for) */
    if (n_kv > g->size / 12 || n_tensors > g->size / 12) goto damaged;
    g->kv = (Kv *)calloc(n_kv ? (size_t)n_kv : 1, sizeof(Kv));
    g->tensors = (Tensor *)calloc(n_tensors ? (size_t)n_tensors : 1, sizeof(Tensor));
    if (!g->kv || !g->tensors) abort();  /* LCOV_EXCL_LINE: out of memory */
    uint64_t alignment = 32;
    for (uint64_t i = 0; i < n_kv; i++) {
        Kv *kv = &g->kv[i];
        kv->key = rd_str(&r);
        uint32_t t = (uint32_t)rd_n(&r, 4);
        if (r.bad) goto damaged;
        if (t >= GV_NTYPES) {
            snprintf(err, errsize, "has metadata '%.*s' of an unknown type (%u)", (int)kv->key.n, kv->key.s, (unsigned)t);
            return 0;
        }
        kv->type = (int)t;
        if (t == GV_ARR) {
            const unsigned char *at = r.p;
            kv->etype = (int)rd_n(&r, 4);
            kv->count = rd_n(&r, 8);
            r.p = at;
        }
        kv->at = t == GV_ARR ? r.p + 12 : r.p;
        rd_skip(&r, t, 0);
        if (r.bad) goto damaged;
        g->n_kv++;
        if (str_is(kv->key, "general.alignment")) {
            alignment = t == GV_U32 ? rd_u32(kv->at) : 0;
            if (alignment == 0 || (alignment & (alignment - 1))) {
                snprintf(err, errsize, "has an alignment that is not a power of 2");
                return 0;
            }
        }
    }
    for (uint64_t i = 0; i < n_tensors; i++) {
        Tensor *t = &g->tensors[i];
        t->name = rd_str(&r);
        uint32_t nd = (uint32_t)rd_n(&r, 4);
        if (r.bad) goto damaged;
        if (nd < 1 || nd > 4) {
            snprintf(err, errsize, "has a tensor '%.*s' of %u dimensions", (int)t->name.n, t->name.s, (unsigned)nd);
            return 0;
        }
        t->ndim = (int)nd;
        int64_t count = 1;
        for (int d = 0; d < 4; d++) {
            uint64_t n = d < t->ndim ? rd_n(&r, 8) : 1;
            if (n == 0 || n > ((uint64_t)1 << 40) || (double)count * (double)n > 9e15) r.bad = 1;
            t->ne[d] = (int64_t)n;
            count *= (int64_t)n;
        }
        uint32_t type = (uint32_t)rd_n(&r, 4);
        uint64_t offset = rd_n(&r, 8);
        if (r.bad) goto damaged;
        if (!type_known(type)) {
            snprintf(err, errsize, "has a tensor '%.*s' of an unknown type (%u)", (int)t->name.n, t->name.s, (unsigned)type);
            return 0;
        }
        t->type = (int)type;
        if (t->ne[0] % type_block(t->type)) {
            snprintf(err, errsize, "has a tensor '%.*s' whose rows are not whole blocks of %s", (int)t->name.n,
                     t->name.s, type_name(t->type));
            return 0;
        }
        t->nbytes = (uint64_t)(type_row_bytes(t->type, t->ne[0]) * (count / t->ne[0]));
        t->data = (const unsigned char *)(uintptr_t)offset;  /* (made a pointer below) */
        g->n_tensors++;
    }
    uint64_t start = (uint64_t)(r.p - g->base);
    start = (start + alignment - 1) / alignment * alignment;
    for (int64_t i = 0; i < g->n_tensors; i++) {
        Tensor *t = &g->tensors[i];
        uint64_t offset = (uint64_t)(uintptr_t)t->data;
        if (offset % alignment || start + offset > g->size || t->nbytes > g->size - start - offset) {
            snprintf(err, errsize, "is cut short or damaged (tensor '%.*s' lies outside it)", (int)t->name.n, t->name.s);
            return 0;
        }
        t->data = g->base + start + offset;
    }
    return 1;
damaged:
    snprintf(err, errsize, "is cut short or damaged");
    return 0;
}

/* Opens and reads a GGUF file: 1, or 0 with a message in err. */
static int gguf_open(Gguf *g, const char *path, int use_map, char *err, size_t errsize) {
    memset(g, 0, sizeof *g);
    if (!(use_map && map_file(g, path)) && !read_file(g, path)) {
        snprintf(err, errsize, "cannot open '%s': %s", path, strerror(errno));
        gguf_free(g);
        return 0;
    }
    char why[200];
    if (gguf_parse(g, why, sizeof why)) return 1;
    snprintf(err, errsize, "'%s' %s", path, why);
    gguf_free(g);
    return 0;
}

/* ---------- metadata ---------- */

static const Kv *kv_find(const Gguf *g, const char *key) {
    for (int64_t i = 0; i < g->n_kv; i++)
        if (str_is(g->kv[i].key, key)) return &g->kv[i];
    return NULL;
}

/* A number of type t at p, as a double. */
static double num_at(int t, const unsigned char *p) {
    uint64_t u;
    uint32_t bits;
    double d;
    float f;
    switch (t) {
    case GV_U8: case GV_BOOL: return p[0];
    case GV_I8: return (int8_t)p[0];
    case GV_U16: return (uint16_t)(p[0] | p[1] << 8);
    case GV_I16: return (int16_t)(uint16_t)(p[0] | p[1] << 8);
    case GV_U32: return rd_u32(p);
    case GV_I32: return (int32_t)rd_u32(p);
    case GV_F32:
        bits = rd_u32(p);
        memcpy(&f, &bits, 4);
        return f;
    case GV_U64: return (double)rd_u64(p);
    case GV_I64: return (double)(int64_t)rd_u64(p);
    default:  /* GV_F64 */
        u = rd_u64(p);
        memcpy(&d, &u, 8);
        return d;
    }
}

static int is_num_type(int t) { return t != GV_STR && t != GV_ARR; }

/* metadata `key` as a number: 1 if it is there and is one */
static int kv_num(const Gguf *g, const char *key, double *out) {
    const Kv *kv = kv_find(g, key);
    if (!kv || !is_num_type(kv->type)) return 0;
    *out = num_at(kv->type, kv->at);
    return 1;
}

/* metadata `key` as text: 1 if it is there and is text */
static int kv_str(const Gguf *g, const char *key, Str *out) {
    const Kv *kv = kv_find(g, key);
    if (!kv || kv->type != GV_STR) return 0;
    out->n = (size_t)rd_u64(kv->at);
    out->s = (const char *)kv->at + 8;
    return 1;
}

/* metadata `key` if it is an array of elements of type et: its Kv, else NULL */
static const Kv *kv_array(const Gguf *g, const char *key, int et) {
    const Kv *kv = kv_find(g, key);
    return kv && kv->type == GV_ARR && kv->etype == et ? kv : NULL;
}

/* An array of strings' strings (malloc'd), or NULL if there are none. */
static Str *kv_strings(const Kv *kv) {
    Str *out = (Str *)malloc(sizeof(Str) * (size_t)(kv->count ? kv->count : 1));
    if (!out) abort();  /* LCOV_EXCL_LINE: out of memory */
    const unsigned char *p = kv->at;
    for (uint64_t i = 0; i < kv->count; i++) {
        out[i].n = (size_t)rd_u64(p);
        out[i].s = (const char *)p + 8;
        p += 8 + out[i].n;
    }
    return out;
}

static const Tensor *tensor_find(const Gguf *g, const char *name) {
    for (int64_t i = 0; i < g->n_tensors; i++)
        if (str_is(g->tensors[i].name, name)) return &g->tensors[i];
    return NULL;
}

/* A value of type t at *p (moved past it) as a Lube value. */
static LubeValue value_at(LubeVM *vm, int t, const unsigned char **p) {
    if (t == GV_STR) {
        size_t n = (size_t)rd_u64(*p);
        LubeValue s = lube->new_str(vm, (const char *)*p + 8, n);
        *p += 8 + n;
        return s;
    }
    if (t == GV_ARR) {
        int et = (int)rd_u32(*p);
        uint64_t n = rd_u64(*p + 4);
        *p += 12;
        LubeValue l = lube->new_list(vm);
        for (uint64_t i = 0; i < n; i++) lube->list_push(l, value_at(vm, et, p));
        return l;
    }
    LubeValue v = t == GV_BOOL ? lube->boolean(**p != 0) : lube->number_value(num_at(t, *p));
    *p += GV_SIZE[t];
    return v;
}

/* All the metadata, as an object. */
static LubeValue gguf_metadata(LubeVM *vm, const Gguf *g) {
    LubeValue o = lube->new_object(vm);
    char key[256];
    for (int64_t i = 0; i < g->n_kv; i++) {
        const Kv *kv = &g->kv[i];
        const unsigned char *p = kv->type == GV_ARR ? kv->at - 12 : kv->at;
        snprintf(key, sizeof key, "%.*s", (int)kv->key.n, kv->key.s);
        lube->object_set(vm, o, key, value_at(vm, kv->type, &p));
    }
    return o;
}
