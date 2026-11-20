/*
 * array/io.c - Arrays in files: numpy's .npy format (save, load), and text
 * with numbers in columns (loadtxt, savetxt). (Part of array.c.)
 */

static const char *const NPY_DESCR[ARR_NDTYPES] = {"<f8", "<f4", "<i8", "<i4", "|u1", "|b1"};

/* save(path, a): a .npy file */
static LubeValue npy_save(LubeVM *vm, const char *path, LubeValue av) {
    Array *a;
    LubeValue keep = as_contiguous(vm, av, -1, &a);
    char header[512], shape[400];
    size_t at = 0;
    for (int d = 0; d < a->ndim; d++) at += (size_t)snprintf(shape + at, sizeof shape - at, "%.0f,%s", (double)a->shape[d], d < a->ndim - 1 ? " " : "");
    if (a->ndim > 1) shape[at - 1] = '\0';  /* ((3,) keeps its comma; (2, 3) has none at the end) */
    if (!a->ndim) shape[0] = '\0';
    int n = snprintf(header, sizeof header, "{'descr': '%s', 'fortran_order': False, 'shape': (%s), }", NPY_DESCR[a->dtype], shape);
    int total = 10 + n + 1, pad = (64 - total % 64) % 64;  /* (the data starts 64-byte aligned) */
    FILE *f = fopen(path, "wb");
    if (!f) {
        lube->release(keep);
        return lube->error(vm, "save(): cannot write '%s'", path);
    }
    unsigned char magic[10] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, (unsigned char)((n + pad + 1) & 0xff), (unsigned char)((n + pad + 1) >> 8)};
    fwrite(magic, 1, sizeof magic, f);
    fwrite(header, 1, (size_t)n, f);
    for (int i = 0; i < pad; i++) fputc(' ', f);
    fputc('\n', f);
    size_t bytes = (size_t)a->size * (size_t)ITEMSIZE[a->dtype];
    int ok = fwrite(a->data, 1, bytes, f) == bytes;
    if (fclose(f)) ok = 0;  /* LCOV_EXCL_LINE: a full disk */
    lube->release(keep);
    return ok ? lube->nothing() : lube->error(vm, "save(): cannot write '%s'", path);  /* LCOV_EXCL_LINE */
}

/* The text after `key` in a .npy header's dictionary ('descr': ...), or NULL. */
static const char *npy_field(const char *header, const char *key) {
    const char *p = strstr(header, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\'' || *p == ':') p++;
    return p;
}

/* load(path): the Array in a .npy file */
static LubeValue npy_load(LubeVM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return lube->error(vm, "load(): cannot read '%s'", path);
    unsigned char magic[12];
    char *header = NULL;
    LubeValue r;
    size_t got = fread(magic, 1, 10, f);
    if (got < 10 || memcmp(magic, "\x93NUMPY", 6)) {
        r = lube->error(vm, "load(): '%s' is not a .npy file", path);
        goto out;
    }
    size_t hlen = magic[8] | (size_t)magic[9] << 8;
    if (magic[6] >= 2) {  /* (versions 2 and 3: a 4-byte length) */
        if (fread(magic + 10, 1, 2, f) != 2) {
            r = lube->error(vm, "load(): '%s' is not a .npy file", path);
            goto out;
        }
        hlen |= (size_t)magic[10] << 16 | (size_t)magic[11] << 24;
    }
    header = (char *)malloc(hlen + 1);
    if (!header || fread(header, 1, hlen, f) != hlen) {
        r = lube->error(vm, "load(): '%s' is not a .npy file", path);
        goto out;
    }
    header[hlen] = '\0';
    const char *descr = npy_field(header, "'descr'"), *fo = npy_field(header, "'fortran_order'"),
               *sh = npy_field(header, "'shape'");
    int dtype = -1;
    for (int t = 0; t < ARR_NDTYPES && descr; t++)
        if (!strncmp(descr + 1, NPY_DESCR[t] + 1, 2) && (descr[0] == '<' || descr[0] == '|' || descr[0] == '=')) dtype = t;
    if (descr && !strncmp(descr, "|i1", 3)) dtype = -1;
    if (dtype < 0 || !fo || !sh || *sh != '(') {
        int len = 0;
        while (descr && descr[len] && descr[len] != '\'' && len < 20) len++;
        r = lube->error(vm, "load(): '%s' holds a kind of Array this cannot read (%.*s)", path, len, descr ? descr : "");
        goto out;
    }
    int nd = 0, fortran = !strncmp(fo, "True", 4);
    int64_t shape[ARR_MAXDIM];
    for (const char *p = sh + 1; *p && *p != ')';) {
        if (*p >= '0' && *p <= '9') {
            if (nd == ARR_MAXDIM) {
                r = lube->error(vm, "load(): '%s' has too many dimensions", path);
                goto out;
            }
            shape[nd++] = strtoll(p, (char **)&p, 10);
        } else {
            p++;
        }
    }
    int64_t rev[ARR_MAXDIM];
    for (int d = 0; d < nd; d++) rev[d] = fortran ? shape[nd - 1 - d] : shape[d];
    Array *a;
    r = arr_new(vm, dtype, nd, rev, &a, 0);
    if (!a) goto out;
    size_t bytes = (size_t)a->size * (size_t)ITEMSIZE[dtype];
    if (fread(a->data, 1, bytes, f) != bytes) {
        lube->release(r);
        r = lube->error(vm, "load(): '%s' ends too soon", path);
        goto out;
    }
    if (fortran) {  /* stored column by column: the reversed shape, transposed */
        Array t = *a;
        for (int d = 0; d < nd; d++) {
            t.shape[d] = a->shape[nd - 1 - d];
            t.strides[d] = a->strides[nd - 1 - d];
        }
        Array *c;
        LubeValue cv = arr_copy(vm, &t, -1, &c);
        lube->release(r);
        r = cv;
    }
out:
    free(header);
    fclose(f);
    return r;
}

/* loadtxt(path, options): numbers in columns, a row a line (delimiter: whitespace unless given; # starts a
 * comment; skiprows lines skipped first); one row or column gives 1 dimension, as numpy's does. */
static LubeValue txt_load(LubeVM *vm, const char *path, const char *delim, int64_t skip, int dtype) {
    FILE *f = fopen(path, "rb");
    if (!f) return lube->error(vm, "loadtxt(): cannot read '%s'", path);
    Text all = {NULL, 0, 0};
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) text_put(&all, chunk, got);
    fclose(f);
    text_str(&all, "");
    double *vals = NULL;
    int64_t nvals = 0, cap = 0, rows = 0, cols = -1, line_no = 0;
    LubeValue r = lube->nothing();
    char *line = all.s;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        line_no++;
        if (line_no > skip) {
            int64_t count = 0;
            char *p = line;
            for (;;) {
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (!*p) break;
                char *stop;
                double x = strtod(p, &stop);
                if (stop == p) {
                    free(vals);
                    free(all.s);
                    return lube->error(vm, "loadtxt(): line %.0f of '%s' has something that is not a number", (double)line_no, path);
                }
                if (nvals == cap) {
                    cap = cap ? cap * 2 : 256;
                    double *grown = (double *)realloc(vals, sizeof(double) * (size_t)cap);
                    if (!grown) abort();  /* LCOV_EXCL_LINE: out of memory */
                    vals = grown;
                }
                vals[nvals++] = x;
                count++;
                p = stop;
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (delim && *delim && !strncmp(p, delim, strlen(delim))) p += strlen(delim);
            }
            if (count) {
                if (cols >= 0 && count != cols) {
                    free(vals);
                    free(all.s);
                    return lube->error(vm, "loadtxt(): line %.0f of '%s' has %.0f numbers, not %.0f as before", (double)line_no, path, (double)count, (double)cols);
                }
                cols = count;
                rows++;
            }
        }
        line = end ? end + 1 : NULL;
    }
    int64_t shape[2] = {rows, cols < 0 ? 0 : cols};
    int nd = 2;
    if (rows <= 1) {
        shape[0] = shape[1];
        nd = 1;
    } else if (cols == 1) {
        nd = 1;
    }
    Array *a;
    r = arr_new(vm, dtype, nd, shape, &a, 0);
    for (int64_t i = 0; i < nvals; i++) store(a->data + i * ITEMSIZE[dtype], dtype, vals[i]);
    free(vals);
    free(all.s);
    return r;
}

/* Whether fmt has exactly one conversion, of a number (%e, %f, %g, %d with flags, width, precision). */
static int number_format(const char *fmt) {
    int conversions = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') {
            p++;
            continue;
        }
        p++;
        while (*p && strchr("-+ #0", *p)) p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (!*p || !strchr("eEfFgGd", *p)) return 0;
        conversions++;
    }
    return conversions == 1;
}

/* savetxt(path, a, options): a row a line (a 1-dimensional Array: a number a line), each number in fmt
 * (numpy's %.18e unless given), between delimiter (a space unless given); header: a first line, after "# ". */
static LubeValue txt_save(LubeVM *vm, const char *path, LubeValue av, const char *fmt, const char *delim, const char *header) {
    Array *a = arr_of(av);
    if (a->ndim < 1 || a->ndim > 2) return lube->error(vm, "savetxt() writes an Array of 1 or 2 dimensions");
    if (!number_format(fmt)) return lube->error(vm, "savetxt(): fmt must have one number in it, such as \"%%.6f\"");
    int is_int = strchr(fmt, 'd') != NULL && !strpbrk(fmt, "eEfFgG");
    FILE *f = fopen(path, "wb");
    if (!f) return lube->error(vm, "savetxt(): cannot write '%s'", path);
    if (header) fprintf(f, "# %s\n", header);
    int64_t rows = a->shape[0], cols = a->ndim == 2 ? a->shape[1] : 1;
    for (int64_t i = 0; i < rows; i++) {
        for (int64_t j = 0; j < cols; j++) {
            const unsigned char *p = a->data + (i * a->strides[0] + (a->ndim == 2 ? j * a->strides[1] : 0)) * ITEMSIZE[a->dtype];
            if (j) fputs(delim, f);
            if (is_int) fprintf(f, fmt, (int)load_int(p, a->dtype));
            else fprintf(f, fmt, load(p, a->dtype));
        }
        fputc('\n', f);
    }
    int ok = !fclose(f);
    return ok ? lube->nothing() : lube->error(vm, "savetxt(): cannot write '%s'", path);  /* LCOV_EXCL_LINE */
}

/* ======================================================================
 * safetensors: a JSON header naming each Array (dtype, shape, where its
 * bytes are), then the bytes. Read: F64, F32, F16 and BF16 (as float32),
 * I64, I32, I16 and I8 (as int32), U8, BOOL. Written: the dtypes Arrays have.
 * ==================================================================== */

static const char *const ST_NAMES[ARR_NDTYPES] = {"F64", "F32", "I64", "I32", "U8", "BOOL"};

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {  /* subnormal: normalized */
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                exp--;
            }
            bits = sign | exp << 23 | (man & 0x3ff) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | man << 13;
    } else {
        bits = sign | (exp + 127 - 15) << 23 | man << 13;
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* A tiny JSON reader, enough for a safetensors header. */
typedef struct {
    const char *p, *end;
} Json;

static void json_space(Json *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

/* A string into buf (truncated to size): 0 if there is none. */
static int json_string(Json *j, char *buf, size_t size) {
    json_space(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    size_t n = 0;
    for (j->p++; j->p < j->end && *j->p != '"'; j->p++) {
        char c = *j->p;
        if (c == '\\' && j->p + 1 < j->end) {
            c = *++j->p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u' && j->p + 4 < j->end) {  /* (\uXXXX: ASCII as itself, others as '?') */
                unsigned v = (unsigned)strtoul((char[5]){j->p[1], j->p[2], j->p[3], j->p[4], 0}, NULL, 16);
                c = v < 128 ? (char)v : '?';
                j->p += 4;
            }
        }
        if (n + 1 < size) buf[n++] = c;
    }
    if (j->p >= j->end) return 0;
    j->p++;
    buf[n] = '\0';
    return 1;
}

static int json_char(Json *j, char c) {
    json_space(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

/* Skips any value: 0 if it is not one. */
static int json_skip(Json *j) {
    char tmp[2];
    json_space(j);
    if (j->p >= j->end) return 0;
    if (*j->p == '"') return json_string(j, tmp, sizeof tmp);
    if (*j->p == '{' || *j->p == '[') {
        char close = *j->p == '{' ? '}' : ']';
        j->p++;
        if (json_char(j, close)) return 1;
        do {
            if (close == '}' && (!json_string(j, tmp, sizeof tmp) || !json_char(j, ':'))) return 0;
            if (!json_skip(j)) return 0;
        } while (json_char(j, ','));
        return json_char(j, close);
    }
    const char *start = j->p;
    while (j->p < j->end && (strchr("-+.eE", *j->p) || (*j->p >= '0' && *j->p <= '9') || (*j->p >= 'a' && *j->p <= 'z'))) j->p++;
    return j->p > start;
}

static int json_int(Json *j, int64_t *out) {
    json_space(j);
    char *stop;
    long long v = strtoll(j->p, &stop, 10);
    if (stop == j->p || stop > j->end) return 0;
    j->p = stop;
    *out = v;
    return 1;
}

/* load_safetensors(path): an object of the Arrays in the file, by name */
static LubeValue st_load(LubeVM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return lube->error(vm, "load_safetensors(): cannot read '%s'", path);
    unsigned char len8[8];
    LubeValue r = lube->nothing();
    char *header = NULL;
    uint64_t hlen = 0;
    if (fread(len8, 1, 8, f) == 8)
        for (int i = 7; i >= 0; i--) hlen = hlen << 8 | len8[i];
    if (hlen == 0 || hlen > 100000000 || !(header = (char *)malloc((size_t)hlen)) || fread(header, 1, (size_t)hlen, f) != hlen) {
        r = lube->error(vm, "load_safetensors(): '%s' is not a safetensors file", path);
        goto out;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    long base = (long)(8 + hlen);
    Json j = {header, header + hlen};
    r = lube->new_object(vm);
    if (!json_char(&j, '{')) goto bad;
    if (json_char(&j, '}')) goto out;
    do {
        char name[512], key[64], dt[16] = "";
        int64_t shape[ARR_MAXDIM], off[2] = {-1, -1};
        int nd = -1;
        if (!json_string(&j, name, sizeof name) || !json_char(&j, ':')) goto bad;
        if (!strcmp(name, "__metadata__")) {
            if (!json_skip(&j)) goto bad;
            continue;
        }
        if (!json_char(&j, '{')) goto bad;
        do {
            if (!json_string(&j, key, sizeof key) || !json_char(&j, ':')) goto bad;
            if (!strcmp(key, "dtype")) {
                if (!json_string(&j, dt, sizeof dt)) goto bad;
            } else if (!strcmp(key, "shape") || !strcmp(key, "data_offsets")) {
                int is_shape = key[0] == 's', n = 0;
                if (!json_char(&j, '[')) goto bad;
                if (!json_char(&j, ']')) {
                    do {
                        int64_t v;
                        if (!json_int(&j, &v) || n >= (is_shape ? ARR_MAXDIM : 2) || v < 0) goto bad;
                        (is_shape ? shape : off)[n++] = v;
                    } while (json_char(&j, ','));
                    if (!json_char(&j, ']')) goto bad;
                }
                if (is_shape) nd = n;
            } else if (!json_skip(&j)) {
                goto bad;
            }
        } while (json_char(&j, ','));
        if (!json_char(&j, '}') || nd < 0 || off[1] < off[0] || off[0] < 0) goto bad;
        /* the stored dtype, its size, and the dtype it becomes */
        static const struct { const char *name; int size, to; } kinds[] = {
            {"F64", 8, ARR_F64}, {"F32", 4, ARR_F32}, {"F16", 2, ARR_F32}, {"BF16", 2, ARR_F32}, {"I64", 8, ARR_I64},
            {"I32", 4, ARR_I32}, {"I16", 2, ARR_I32}, {"I8", 1, ARR_I32}, {"U8", 1, ARR_U8}, {"BOOL", 1, ARR_BOOL}};
        int k = -1;
        for (int i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++)
            if (!strcmp(dt, kinds[i].name)) k = i;
        if (k < 0) {
            lube->release(r);
            r = lube->error(vm, "load_safetensors(): '%s' has '%s' of dtype %s, which this cannot read", path, name, dt);
            goto out;
        }
        int64_t n = shape_size(nd, shape);
        if (n < 0 || off[1] - off[0] != n * kinds[k].size || base + off[1] > total) goto bad;
        Array *a;
        LubeValue av = arr_new(vm, kinds[k].to, nd, shape, &a, 0);
        unsigned char *raw = (unsigned char *)malloc((size_t)(n * kinds[k].size + 1));
        if (!raw) abort();  /* LCOV_EXCL_LINE: out of memory */
        fseek(f, base + (long)off[0], SEEK_SET);
        if (fread(raw, 1, (size_t)(n * kinds[k].size), f) != (size_t)(n * kinds[k].size)) {  /* LCOV_EXCL_START: (its size was checked) */
            free(raw);
            lube->release(av);
            goto bad;
        }  /* LCOV_EXCL_STOP */
        const char *kn = kinds[k].name;
        if (kinds[k].size == ITEMSIZE[kinds[k].to]) {  /* stored as it is held */
            if (n) memcpy(a->data, raw, (size_t)(n * kinds[k].size));
        } else {
            for (int64_t i = 0; i < n; i++) {
                unsigned char *p = raw + i * kinds[k].size;
                uint32_t bits = (uint32_t)(p[0] | (kinds[k].size > 1 ? p[1] << 8 : 0));
                if (!strcmp(kn, "F16")) {
                    ((float *)a->data)[i] = half_to_float((uint16_t)bits);
                } else if (!strcmp(kn, "BF16")) {
                    bits <<= 16;
                    memcpy((float *)a->data + i, &bits, 4);
                } else {
                    ((int32_t *)a->data)[i] = kinds[k].size == 2 ? (int16_t)bits : (int8_t)bits;
                }
            }
        }
        free(raw);
        lube->object_set(vm, r, name, av);
    } while (json_char(&j, ','));
    if (json_char(&j, '}')) goto out;
bad:
    lube->release(r);
    r = lube->error(vm, "load_safetensors(): '%s' is not a safetensors file", path);
out:
    free(header);
    fclose(f);
    return r;
}

/* save_safetensors(path, arrays): an object of Arrays by name, written in the order given */
static LubeValue st_save(LubeVM *vm, const char *path, LubeValue obj, int count, const char *const *names, Array *const *arrays) {
    (void)obj;
    Text h = {NULL, 0, 0};
    text_str(&h, "{");
    int64_t at = 0;
    for (int i = 0; i < count; i++) {
        char item[1200], shape[400] = "";
        size_t sl = 0;
        for (int d = 0; d < arrays[i]->ndim; d++) sl += (size_t)snprintf(shape + sl, sizeof shape - sl, "%s%.0f", d ? "," : "", (double)arrays[i]->shape[d]);
        int64_t bytes = arrays[i]->size * ITEMSIZE[arrays[i]->dtype];
        text_str(&h, i ? "," : "");
        text_str(&h, "\"");
        for (const char *c = names[i]; *c; c++) {  /* (a name's quotes and backslashes escaped) */
            if (*c == '"' || *c == '\\') text_str(&h, "\\");
            text_put(&h, c, 1);
        }
        snprintf(item, sizeof item, "\":{\"dtype\":\"%s\",\"shape\":[%s],\"data_offsets\":[%.0f,%.0f]}", ST_NAMES[arrays[i]->dtype], shape, (double)at, (double)(at + bytes));
        text_str(&h, item);
        at += bytes;
    }
    text_str(&h, "}");
    while (h.len % 8) text_str(&h, " ");  /* (the data 8-byte aligned) */
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(h.s);
        return lube->error(vm, "save_safetensors(): cannot write '%s'", path);
    }
    unsigned char len8[8];
    for (int i = 0; i < 8; i++) len8[i] = (unsigned char)((uint64_t)h.len >> (8 * i));
    fwrite(len8, 1, 8, f);
    fwrite(h.s, 1, h.len, f);
    free(h.s);
    for (int i = 0; i < count; i++) {
        Array *c;
        LubeValue keep = arr_copy(vm, arrays[i], -1, &c);
        fwrite(c->data, 1, (size_t)(c->size * ITEMSIZE[c->dtype]), f);
        lube->release(keep);
    }
    int ok = !fclose(f);
    return ok ? lube->nothing() : lube->error(vm, "save_safetensors(): cannot write '%s'", path);  /* LCOV_EXCL_LINE */
}
