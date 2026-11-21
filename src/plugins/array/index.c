/*
 * array/index.c - a[...]: reading and changing parts of an Array, as numpy
 * does. Numbers pick one position (a[1, 2]), ranges pick a slice (a[1..],
 * a[.., 0], a[range(0, 10, 2)]), both giving a view; lists or Arrays of
 * whole numbers pick those positions (a[[0, 2]]), and a bool Array picks
 * where it is true (a[a > 0]), both giving a copy. (Part of array.c.)
 */

enum { IX_INT, IX_SLICE, IX_ARRAY };

typedef struct {
    int kind;
    int64_t i;                   /* IX_INT: the position (already counted from the start) */
    int64_t start, step, len;    /* IX_SLICE: resolved against the axis */
    Array *idx;                  /* IX_ARRAY: int64 positions (any shape) */
    LubeValue keep;              /* (what to let go of: an Array made for it) */
    int axis;                    /* the axis of a it indexes */
} IndexItem;

typedef struct {
    IndexItem items[ARR_MAXDIM];
    int n;
    int arrays;                  /* how many IX_ARRAY items */
} Index;

static void index_done(Index *ix) {
    for (int i = 0; i < ix->n; i++)
        if (ix->items[i].kind == IX_ARRAY) lube->release(ix->items[i].keep);
}

/* A range resolved against an axis of length n, as Python resolves a slice (negative ends count from the end,
 * ends past it stop there). */
static void resolve_slice(double s, double e, double step, int incl, int64_t n, IndexItem *it) {
    it->kind = IX_SLICE;
    it->step = (int64_t)step;
    double len = (double)n;
    if (step > 0) {
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (incl) e += 1;
        s = s < 0 ? 0 : s > len ? len : s;
        e = e < 0 ? 0 : e > len ? len : e;
        it->start = (int64_t)s;
        it->len = e > s ? (int64_t)ceil((e - s) / step) : 0;
    } else {
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (incl) e -= 1;
        s = s < -1 ? -1 : s > len - 1 ? len - 1 : s;
        e = e < -1 ? -1 : e > len - 1 ? len - 1 : e;
        it->start = (int64_t)s;
        it->len = s > e ? (int64_t)ceil((s - e) / -step) : 0;
    }
}

/* A whole number in -n..n-1 as a position on an axis of length n; -1 after an error. */
static int64_t position(LubeVM *vm, double d, int64_t n, int axis, LubeValue *err) {
    if (d != floor(d)) {
        *err = lube->error(vm, "an Array index must be a whole number, not %g", d);
        return -1;
    }
    double k = d < 0 ? d + (double)n : d;
    if (!(k >= 0 && k < (double)n)) {
        char num[64];
        lube->format_number(d, num, sizeof num);
        *err = lube->error(vm, "index %s is out of range for axis %d (length %.0f)", num, axis, (double)n);
        return -1;
    }
    return (int64_t)k;
}

/* Reads the indexes (argv[1..argc-1]) against a. 0 after an error (in *err). borrow: an Array of positions
 * that needs no change (int64, in a row, none negative or past the end) is used as it is, not copied: for
 * reading only, since a[a] = v would change the positions it goes by. */
static int index_parse(LubeVM *vm, const Array *a, int argc, const LubeValue *argv, Index *ix, LubeValue *err, int borrow) {
    ix->n = 0;
    ix->arrays = 0;
    int axis = 0;
    for (int k = 1; k < argc; k++) {
        LubeValue v = argv[k];
        int kind = lube->kind(v);
        double s, e, st;
        int incl;
        Array *ia = arr_of(v);
        int mask_dims = 0;
        Array *mask = NULL;
        LubeValue made = lube->nothing();
        if (!ia && kind == LUBE_LIST) {  /* a list: positions, or Bools (a mask) */
            made = arr_from(vm, v, -1, &ia);
            if (!ia) {
                *err = made;
                index_done(ix);
                return 0;
            }
        }
        if (ia && ia->dtype == ARR_BOOL) {
            mask = ia;
            mask_dims = ia->ndim;
        }
        int need = mask ? (mask_dims ? mask_dims : 1) : 1;
        if (axis + need > a->ndim || ix->n + need > ARR_MAXDIM) {
            lube->release(made);
            index_done(ix);
            *err = lube->error(vm, "too many indexes for an Array of %d dimension%s", a->ndim, a->ndim == 1 ? "" : "s");
            return 0;
        }
        if (mask) {  /* where it is true: one Array of positions for each of its axes */
            for (int d = 0; d < mask_dims; d++)
                if (mask->shape[d] != a->shape[axis + d]) {
                    char x[200], y[200];
                    shape_text(x, sizeof x, mask->ndim, mask->shape);
                    shape_text(y, sizeof y, a->ndim, a->shape);
                    lube->release(made);
                    index_done(ix);
                    *err = lube->error(vm, "a bool index of shape %s does not fit an Array of shape %s", x, y);
                    return 0;
                }
            int64_t count = 0;
            for (int64_t i = 0; i < mask->size; i++) count += *elem_at(mask, i) != 0;
            for (int d = 0; d < mask_dims; d++) {
                IndexItem *it = &ix->items[ix->n++];
                it->kind = IX_ARRAY;
                it->axis = axis + d;
                it->keep = arr_new(vm, ARR_I64, 1, &count, &it->idx, 0);
            }
            int64_t at = 0;
            for (int64_t i = 0; i < mask->size; i++) {
                if (!*elem_at(mask, i)) continue;
                int64_t r = i;
                for (int d = mask_dims - 1; d >= 0; d--) {
                    ((int64_t *)ix->items[ix->n - mask_dims + d].idx->data)[at] = r % mask->shape[d];
                    r /= mask->shape[d];
                }
                at++;
            }
            ix->arrays += mask_dims;
            axis += mask_dims;
            lube->release(made);
            continue;
        }
        IndexItem *it = &ix->items[ix->n];
        it->axis = axis;
        if (ia) {  /* positions */
            it->kind = IX_ARRAY;
            int64_t len = a->shape[axis];
            int fine = ia->dtype == ARR_I64 && is_contiguous(ia);
            if (fine && borrow) {
                const int64_t *in = (const int64_t *)ia->data;
                int outside = 0;
                for (int64_t i = 0; i < ia->size; i++) outside |= (uint64_t)in[i] >= (uint64_t)len;
                if (!outside) {
                    it->idx = ia;
                    it->keep = lube->kind(made) == LUBE_NOTHING ? lube->retain(v) : made;
                    ix->n++;
                    ix->arrays++;
                    axis++;
                    continue;
                }
            }
            it->keep = arr_new(vm, ARR_I64, ia->ndim, ia->shape, &it->idx, 0);
            ix->n++;
            int64_t *out = (int64_t *)it->idx->data;
            if (fine) {  /* (whole numbers in a row: counted from the end when negative, checked all at once) */
                const int64_t *in = (const int64_t *)ia->data;
                for (int64_t i = 0; i < ia->size; i++) {
                    int64_t k = in[i] < 0 ? in[i] + len : in[i];
                    fine &= k >= 0 && k < len;
                    out[i] = k;
                }
            }
            for (int64_t i = 0; i < ia->size && !fine; i++) {  /* (the general way, which gives the errors) */
                double d = load(elem_at(ia, i), ia->dtype);
                int64_t p = position(vm, d, a->shape[axis], axis, err);
                if (p < 0) {
                    lube->release(made);
                    index_done(ix);
                    return 0;
                }
                ((int64_t *)it->idx->data)[i] = p;
            }
            ix->arrays++;
            lube->release(made);
        } else if (kind == LUBE_NUMBER) {
            int64_t p = position(vm, lube->number(v), a->shape[axis], axis, err);
            if (p < 0) {
                index_done(ix);
                return 0;
            }
            it->kind = IX_INT;
            it->i = p;
            ix->n++;
        } else if (lube->range(v, &s, &e, &st, &incl)) {
            if (st != floor(st) || (s != floor(s) && !isinf(s)) || (e != floor(e) && !isinf(e))) {
                index_done(ix);
                *err = lube->error(vm, "an Array slice needs whole numbers");
                return 0;
            }
            resolve_slice(s, e, st, incl, a->shape[axis], it);
            ix->n++;
        } else {
            index_done(ix);
            *err = lube->error(vm, "an Array index must be a Number, a range, a list or an Array, not %s", lube->a_type(v));
            return 0;
        }
        axis++;
    }
    while (axis < a->ndim) {  /* the rest: whole */
        IndexItem *it = &ix->items[ix->n++];
        it->axis = axis;
        it->kind = IX_SLICE;
        it->start = 0;
        it->step = 1;
        it->len = a->shape[axis++];
    }
    return 1;
}

/* The view picked by an index of numbers and slices only (no arrays): offset, shape and strides into a. */
static void basic_view(const Array *a, const Index *ix, Array *v) {
    *v = *a;
    v->ndim = 0;
    int64_t off = 0;
    for (int k = 0; k < ix->n; k++) {
        const IndexItem *it = &ix->items[k];
        int64_t st = a->strides[it->axis];
        if (it->kind == IX_INT) {
            off += it->i * st;
        } else {
            off += it->start * st;
            v->shape[v->ndim] = it->len;
            v->strides[v->ndim++] = st * it->step;
        }
    }
    v->data = a->data + off * ITEMSIZE[a->dtype];
    set_size(v);
}

/* The advanced index's parts: the shape its arrays broadcast to (numbers count as arrays of one), the slices'
 * shape and strides, and where the arrays' axes go in the result (in front, unless they sit together after
 * some slices: then there). */
typedef struct {
    int nb, ns;
    int64_t bshape[ARR_MAXDIM], sshape[ARR_MAXDIM], sstride[ARR_MAXDIM];
    int64_t base_off;
    int at;  /* how many slice axes come before the arrays' in the result */
} Advanced;

static int advanced_plan(LubeVM *vm, const Array *a, const Index *ix, Advanced *p, LubeValue *err) {
    p->nb = 0;
    p->ns = 0;
    p->base_off = 0;
    int first = -1, last = -1, slices_before = 0;
    for (int k = 0; k < ix->n; k++) {
        const IndexItem *it = &ix->items[k];
        if (it->kind == IX_SLICE) {
            p->base_off += it->start * a->strides[it->axis];
            p->sshape[p->ns] = it->len;
            p->sstride[p->ns++] = a->strides[it->axis] * it->step;
            if (first < 0) slices_before++;
            continue;
        }
        if (first < 0) first = k;
        last = k;
        if (it->kind == IX_ARRAY) {  /* broadcast into bshape */
            const Array *ia = it->idx;
            int n = p->nb > ia->ndim ? p->nb : ia->ndim;
            int64_t shape[ARR_MAXDIM];
            for (int i = 0; i < n; i++) {
                int da = p->nb - n + i, db = ia->ndim - n + i;
                int64_t x = da >= 0 ? p->bshape[da] : 1, y = db >= 0 ? ia->shape[db] : 1;
                if (x != y && x != 1 && y != 1) {
                    *err = lube->error(vm, "the index arrays' shapes do not broadcast together");
                    return 0;
                }
                shape[i] = x == 1 ? y : x;
            }
            p->nb = n;
            memcpy(p->bshape, shape, sizeof(int64_t) * (size_t)n);
        }
    }
    int together = 1;
    for (int k = first; k <= last; k++) together &= ix->items[k].kind != IX_SLICE;
    p->at = together ? slices_before : 0;
    return 1;
}

/* The offset (in elements) of the arrays' and numbers' part for broadcast position l. */
static int64_t advanced_offset(const Array *a, const Index *ix, const Advanced *p, int64_t l) {
    int64_t pos[ARR_MAXDIM], r = l, off = 0;
    for (int d = p->nb - 1; d >= 0; d--) {
        pos[d] = r % p->bshape[d];
        r /= p->bshape[d];
    }
    for (int k = 0; k < ix->n; k++) {
        const IndexItem *it = &ix->items[k];
        if (it->kind == IX_INT) {
            off += it->i * a->strides[it->axis];
        } else if (it->kind == IX_ARRAY) {
            const Array *ia = it->idx;
            int64_t e = 0;
            for (int d = 0; d < ia->ndim; d++) {
                int bd = p->nb - ia->ndim + d;
                e += (ia->shape[d] == 1 ? 0 : pos[bd]) * ia->strides[d];
            }
            off += ((const int64_t *)ia->data)[e] * a->strides[it->axis];
        }
    }
    return off;
}

/* The index array, when it is the only one and no Number is among the indexes: then position l's offset is
 * base_off + idx[l] * the axis's stride (the array is its own broadcast shape, C-contiguous: index_parse made
 * it). Else NULL. */
static const IndexItem *single_array(const Index *ix, const Advanced *p) {
    const IndexItem *one = NULL;
    for (int k = 0; k < ix->n; k++) {
        if (ix->items[k].kind == IX_INT) return NULL;
        if (ix->items[k].kind == IX_ARRAY) one = &ix->items[k];
    }
    return ix->arrays == 1 && one && p->nb == one->idx->ndim ? one : NULL;
}

/* The order of the result's axes from [arrays' axes, slices' axes] to where they go. */
static void advanced_axes(const Advanced *p, int *axes) {
    int k = 0;
    for (int i = 0; i < p->at; i++) axes[k++] = p->nb + i;
    for (int i = 0; i < p->nb; i++) axes[k++] = i;
    for (int i = p->at; i < p->ns; i++) axes[k++] = p->nb + i;
}

/* A view of a with its axes put in this order. */
static void permuted(const Array *a, const int *axes, Array *v) {
    *v = *a;
    for (int d = 0; d < a->ndim; d++) {
        v->shape[d] = a->shape[axes[d]];
        v->strides[d] = a->strides[axes[d]];
    }
}

/* a[...] */
/* ---------- a[mask], the mask a bool Array of a's own shape, both C-contiguous: numpy's most common case,
 * done directly, in chunks (counted, then copied without branches), on several threads when it is big ---------- */

#define MASK_CHUNK 16384

typedef struct {
    const unsigned char *m, *src;
    unsigned char *dst;
    int64_t n, z;     /* elements, item size */
    int64_t *at;      /* each chunk's trues; then where its elements go */
} MaskJob;

static void mask_count(void *ctx, int64_t from, int64_t to) {
    MaskJob *j = (MaskJob *)ctx;
    for (int64_t c = from; c < to; c++) {
        int64_t s = c * MASK_CHUNK, e = s + MASK_CHUNK < j->n ? s + MASK_CHUNK : j->n, k = 0;
        for (int64_t i = s; i < e; i++) k += j->m[i] != 0;
        j->at[c] = k;
    }
}

/* (each write lands at the next free place; only the true ones move on from it: up to the chunk's last true one,
 * so that no write passes the chunk's share) */
#define MASK_COPY(T)                                                                                               do {                                                                                                               const T *src_ = (const T *)j->src;                                                                             T *dst_ = (T *)j->dst + j->at[c];                                                                              for (int64_t i = s, k = 0; i <= last; i++) {                                                                      dst_[k] = src_[i];                                                                                             k += j->m[i] != 0;                                                                                         }                                                                                                          } while (0)

static void mask_copy(void *ctx, int64_t from, int64_t to) {
    MaskJob *j = (MaskJob *)ctx;
    for (int64_t c = from; c < to; c++) {
        int64_t s = c * MASK_CHUNK, e = s + MASK_CHUNK < j->n ? s + MASK_CHUNK : j->n, last = e - 1;
        while (last >= s && !j->m[last]) last--;
        if (j->z == 8) MASK_COPY(uint64_t);
        else if (j->z == 4) MASK_COPY(uint32_t);
        else MASK_COPY(unsigned char);
    }
}

/* The mask, if index v is a bool Array of a's shape, both C-contiguous; else NULL. */
static Array *whole_mask(const Array *a, LubeValue v) {
    Array *m = arr_of(v);
    if (!m || m->dtype != ARR_BOOL || m->ndim != a->ndim || a->ndim == 0 || !is_contiguous(m) || !is_contiguous(a))
        return NULL;
    for (int d = 0; d < a->ndim; d++)
        if (m->shape[d] != a->shape[d]) return NULL;
    return m;
}

static LubeValue masked(LubeVM *vm, const Array *a, const Array *m) {
    int64_t n = a->size, chunks = (n + MASK_CHUNK - 1) / MASK_CHUNK, count = 0;
    MaskJob j = {m->data, a->data, NULL, n, ITEMSIZE[a->dtype], (int64_t *)malloc(sizeof(int64_t) * (size_t)(chunks + 1))};
    if (!j.at) abort();  /* LCOV_EXCL_LINE: out of memory */
    int64_t grain = PARALLEL_WORK / MASK_CHUNK;
    par_for(chunks, grain, mask_count, &j);
    for (int64_t c = 0; c < chunks; c++) {
        int64_t k = j.at[c];
        j.at[c] = count;
        count += k;
    }
    Array *r;
    LubeValue rv = arr_new(vm, a->dtype, 1, &count, &r, 0);
    j.dst = r->data;
    par_for(chunks, grain, mask_copy, &j);
    free(j.at);
    return rv;
}

/* One element's offset (in elements) when idx is a whole number in range for each of a's axes: 1; else 0, and the
 * general way takes it (and gives the errors). */
static int element_at(const Array *a, int nidx, const LubeValue *idx, int64_t *off) {
    if (nidx != a->ndim || nidx == 0) return 0;
    int64_t at = 0;
    for (int d = 0; d < nidx; d++) {
        if (lube->kind(idx[d]) != LUBE_NUMBER) return 0;
        double x = lube->number(idx[d]), k = x < 0 ? x + (double)a->shape[d] : x;
        if (!(k >= 0 && k < (double)a->shape[d]) || x != floor(x)) return 0;
        at += (int64_t)k * a->strides[d];
    }
    *off = at;
    return 1;
}

static LubeValue index_get(LubeVM *vm, int argc, const LubeValue *argv) {
    Array *a = arr_of(argv[0]);
    Index ix;
    LubeValue err;
    int64_t off;
    if (element_at(a, argc - 1, argv + 1, &off)) return elem_value(a->data + off * ITEMSIZE[a->dtype], a->dtype);
    Array *mask = argc == 2 ? whole_mask(a, argv[1]) : NULL;
    if (mask) return masked(vm, a, mask);
    if (!index_parse(vm, a, argc, argv, &ix, &err, 1)) return err;
    LubeValue r;
    if (!ix.arrays) {
        Array v;
        basic_view(a, &ix, &v);
        if (v.ndim == 0) {
            r = elem_value(v.data, v.dtype);  /* LCOV_EXCL_LINE: (element_at takes every such index first) */
        } else {
            Array *out;
            r = arr_view(vm, a, &out);
            *out = v;
        }
        index_done(&ix);
        return r;
    }
    Advanced p;
    if (!advanced_plan(vm, a, &ix, &p, &err)) {
        index_done(&ix);
        return err;
    }
    if (p.nb + p.ns > ARR_MAXDIM) {
        index_done(&ix);
        return lube->error(vm, "that index would make too many dimensions");
    }
    int64_t shape[ARR_MAXDIM];
    memcpy(shape, p.bshape, sizeof(int64_t) * (size_t)p.nb);
    memcpy(shape + p.nb, p.sshape, sizeof(int64_t) * (size_t)p.ns);
    Array *g;
    r = arr_new(vm, a->dtype, p.nb + p.ns, shape, &g, 0);
    if (!g) {
        index_done(&ix);
        return r;
    }
    int64_t L = shape_size(p.nb, p.bshape), block = shape_size(p.ns, p.sshape);
    Array src = *a, dst = *g;
    src.ndim = dst.ndim = p.ns;
    memcpy(src.shape, p.sshape, sizeof(int64_t) * (size_t)p.ns);
    memcpy(src.strides, p.sstride, sizeof(int64_t) * (size_t)p.ns);
    memcpy(dst.shape, p.sshape, sizeof(int64_t) * (size_t)p.ns);
    c_strides(&dst);
    const IndexItem *one = single_array(&ix, &p);
    int z = ITEMSIZE[a->dtype];
    if (one && block == 1) {  /* (a[positions]: each one element, gathered directly) */
        const int64_t *idx = (const int64_t *)one->idx->data;
        int64_t st = a->strides[one->axis];
        const unsigned char *base = a->data + p.base_off * z;
        if (z == 8) for (int64_t l = 0; l < L; l++) ((uint64_t *)g->data)[l] = *(const uint64_t *)(const void *)(base + idx[l] * st * 8);
        else if (z == 4) for (int64_t l = 0; l < L; l++) ((uint32_t *)g->data)[l] = *(const uint32_t *)(const void *)(base + idx[l] * st * 4);
        else for (int64_t l = 0; l < L; l++) g->data[l] = base[idx[l] * st];
    } else {
        int rows = one && is_contiguous(&src) && block > 1;  /* (whole rows: copied as blocks) */
        for (int64_t l = 0; l < L && block; l++) {
            int64_t off = one ? p.base_off + ((const int64_t *)one->idx->data)[l] * a->strides[one->axis]
                              : p.base_off + advanced_offset(a, &ix, &p, l);
            src.data = a->data + off * z;
            dst.data = g->data + l * block * z;
            if (rows) memcpy(dst.data, src.data, (size_t)(block * z));
            else copy_into(&dst, &src);
        }
    }
    if (p.at > 0) {  /* the arrays' axes go after the first slices */
        int axes[ARR_MAXDIM];
        advanced_axes(&p, axes);
        Array moved;
        permuted(g, axes, &moved);
        Array *c;
        LubeValue cv = arr_copy(vm, &moved, -1, &c);
        lube->release(r);
        r = cv;
    }
    index_done(&ix);
    return r;
}

/* a[...] = v (argv: a, the indexes, v): changes a (its memory its own first). */
static LubeValue index_set(LubeVM *vm, int argc, const LubeValue *argv) {
    Array *a = arr_of(argv[0]);
    Operand o;
    LubeValue err;
    int64_t off;
    if (lube->kind(argv[argc - 1]) == LUBE_NUMBER && element_at(a, argc - 2, argv + 1, &off)) {  /* a[i, j] = x */
        arr_own(a);
        element_at(a, argc - 2, argv + 1, &off);  /* (its memory its own now: perhaps laid out anew) */
        store(a->data + off * ITEMSIZE[a->dtype], a->dtype, lube->number(argv[argc - 1]));
        return lube->nothing();
    }
    if (!operand_of(vm, argv[argc - 1], &o, &err)) return err;
    Array *val;
    LubeValue kv = o.a ? lube->retain(lube->nothing()) : scalar_array(vm, o.num, a->dtype, &val);
    if (o.a) val = o.a;
    Index ix;
    LubeValue r = lube->nothing();
    Array *mask = argc == 3 ? whole_mask(a, argv[1]) : NULL;
    if (mask && val->size == 1 && val->ndim <= 1 && val->dtype == a->dtype) {  /* a[mask] = x: x wherever the mask is true */
        arr_own(a);
        const unsigned char *m = mask->data;
        int64_t n = a->size;
        if (a->dtype == ARR_F64 || a->dtype == ARR_I64) {
            uint64_t x, *d = (uint64_t *)a->data;
            memcpy(&x, val->data, 8);
            for (int64_t i = 0; i < n; i++) d[i] = m[i] ? x : d[i];
        } else if (a->dtype == ARR_F32 || a->dtype == ARR_I32) {
            uint32_t x, *d = (uint32_t *)a->data;
            memcpy(&x, val->data, 4);
            for (int64_t i = 0; i < n; i++) d[i] = m[i] ? x : d[i];
        } else {
            unsigned char x = *val->data, *d = a->data;
            for (int64_t i = 0; i < n; i++) d[i] = m[i] ? x : d[i];
        }
        goto out;
    }
    if (!index_parse(vm, a, argc - 1, argv, &ix, &err, 0)) {
        r = err;
        goto out;
    }
    arr_own(a);
    if (!ix.arrays) {
        Array v;
        basic_view(a, &ix, &v);
        int nd;
        int64_t shape[ARR_MAXDIM];
        if (!broadcast(&v, val, &nd, shape) || nd != v.ndim || memcmp(shape, v.shape, sizeof(int64_t) * (size_t)nd)) {
            r = broadcast_error(vm, val, &v);
        } else {
            copy_into(&v, val);
        }
        index_done(&ix);
        goto out;
    }
    Advanced p;
    if (!advanced_plan(vm, a, &ix, &p, &err) || p.nb + p.ns > ARR_MAXDIM) {
        r = p.nb + p.ns > ARR_MAXDIM ? lube->error(vm, "that index would make too many dimensions") : err;
        index_done(&ix);
        goto out;
    }
    /* the value, broadcast to the result's shape, and seen in [arrays' axes, slices' axes] order */
    int axes[ARR_MAXDIM];
    advanced_axes(&p, axes);
    Array want;
    want.ndim = p.nb + p.ns;
    for (int d = 0; d < want.ndim; d++) want.shape[d] = d < p.nb ? p.bshape[d] : p.sshape[d - p.nb];
    Array final = want;  /* (in the result's order) */
    for (int d = 0; d < want.ndim; d++) final.shape[d] = want.shape[axes[d]];
    int nd;
    int64_t shape[ARR_MAXDIM];
    if (!broadcast(&final, val, &nd, shape) || nd != final.ndim ||
        memcmp(shape, final.shape, sizeof(int64_t) * (size_t)nd)) {
        r = broadcast_error(vm, val, &final);
        index_done(&ix);
        goto out;
    }
    Array *full;
    LubeValue kf = arr_new(vm, a->dtype, final.ndim, final.shape, &full, 0);
    copy_into(full, val);
    int back[ARR_MAXDIM];  /* the inverse order */
    for (int d = 0; d < want.ndim; d++) back[axes[d]] = d;
    Array ordered;
    permuted(full, back, &ordered);
    int64_t L = shape_size(p.nb, p.bshape), block = shape_size(p.ns, p.sshape);
    Array dst = *a, src = ordered;
    dst.ndim = src.ndim = p.ns;
    memcpy(dst.shape, p.sshape, sizeof(int64_t) * (size_t)p.ns);
    memcpy(dst.strides, p.sstride, sizeof(int64_t) * (size_t)p.ns);
    for (int d = 0; d < p.ns; d++) {
        src.shape[d] = ordered.shape[p.nb + d];
        src.strides[d] = ordered.strides[p.nb + d];
    }
    const IndexItem *one = single_array(&ix, &p);
    int z = ITEMSIZE[a->dtype];
    if (one && block == 1 && p.nb == 1) {  /* (a[positions] = values: each one element, directly) */
        const int64_t *idx = (const int64_t *)one->idx->data;
        int64_t st = a->strides[one->axis], sst = ordered.strides[0];
        unsigned char *base = a->data + p.base_off * z, *src = ordered.data;
        if (z == 8)
            for (int64_t l = 0; l < L; l++)
                *(uint64_t *)(void *)(base + idx[l] * st * 8) = *(const uint64_t *)(const void *)(src + l * sst * 8);
        else if (z == 4)
            for (int64_t l = 0; l < L; l++)
                *(uint32_t *)(void *)(base + idx[l] * st * 4) = *(const uint32_t *)(const void *)(src + l * sst * 4);
        else
            for (int64_t l = 0; l < L; l++) base[idx[l] * st] = src[l * sst];
    } else {
        for (int64_t l = 0; l < L && block; l++) {
            int64_t soff = 0, r2 = l;
            for (int d = p.nb - 1; d >= 0; d--) {
                soff += (r2 % ordered.shape[d]) * ordered.strides[d];
                r2 /= ordered.shape[d];
            }
            int64_t off = one ? p.base_off + ((const int64_t *)one->idx->data)[l] * a->strides[one->axis]
                              : p.base_off + advanced_offset(a, &ix, &p, l);
            dst.data = a->data + off * z;
            src.data = ordered.data + soff * z;
            copy_into(&dst, &src);
        }
    }
    lube->release(kf);
    index_done(&ix);
out:
    lube->release(kv);
    operand_done(&o);
    return r;
}
