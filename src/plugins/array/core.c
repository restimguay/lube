/*
 * array/core.c - Arrays: their memory, dtypes and shapes; loops over their
 * elements; Arrays made from Lube's values and back; how they print.
 * (Part of array.c.)
 */

struct ArrBuffer {
    int refs;             /* the Arrays viewing it (they stay in one isolate: no lock) */
    unsigned char *data;  /* 64-byte aligned, after this header */
};

static const char *const DTYPE_NAMES[ARR_NDTYPES] = {"float64", "float32", "int64", "int32", "uint8", "bool"};
static const int ITEMSIZE[ARR_NDTYPES] = {8, 4, 8, 4, 1, 1};
#define IS_FLOAT(t) ((t) <= ARR_F32)
#define MAX_ELEMENTS ((int64_t)1 << 40)  /* (far beyond any memory: sizes stay clear of overflow) */

static LubeType *ArrayType;

/* A dtype by name (also "float" and "int"), or -1. */
static int dtype_named(const char *s) {
    for (int t = 0; t < ARR_NDTYPES; t++)
        if (!strcmp(s, DTYPE_NAMES[t])) return t;
    if (!strcmp(s, "float")) return ARR_F64;
    if (!strcmp(s, "int")) return ARR_I64;
    return -1;
}

/* v's Array, or NULL. */
static Array *arr_of(LubeValue v) { return (Array *)lube->foreign(v, ArrayType); }

static void arr_release(void *data) {
    Array *a = (Array *)data;
    if (a->buf && --a->buf->refs == 0) free(a->buf);
}

static ArrBuffer *buffer_new(size_t bytes, int zero) {
    size_t total = sizeof(ArrBuffer) + 64 + bytes;
    char *raw = (char *)(zero ? calloc(1, total) : malloc(total));
    if (!raw) return NULL;  /* LCOV_EXCL_LINE: out of memory */
    ArrBuffer *b = (ArrBuffer *)raw;
    uintptr_t p = (uintptr_t)(raw + sizeof(ArrBuffer));
    b->data = (unsigned char *)((p + 63) & ~(uintptr_t)63);
    b->refs = 1;
    return b;
}

static void c_strides(Array *a) {
    int64_t s = 1;
    for (int d = a->ndim - 1; d >= 0; d--) {
        a->strides[d] = s;
        s *= a->shape[d];
    }
}

/* Whether a's elements lie one after another in C order. */
static int is_contiguous(const Array *a) {
    int64_t s = 1;
    for (int d = a->ndim - 1; d >= 0; d--) {
        if (a->shape[d] != 1 && a->strides[d] != s) return 0;
        s *= a->shape[d];
    }
    return 1;
}

/* The product of a shape: -1 if it is too big. */
static int64_t shape_size(int ndim, const int64_t *shape) {
    int64_t n = 1;
    for (int d = 0; d < ndim; d++) {
        if (shape[d] && n > MAX_ELEMENTS / shape[d]) return -1;
        n *= shape[d];
    }
    return n;
}

/* A new Array (zeros if zero), C-contiguous: its object, and *out. An error if it is too big. */
static LubeValue arr_new(LubeVM *vm, int dtype, int ndim, const int64_t *shape, Array **out, int zero) {
    *out = NULL;
    int64_t n = shape_size(ndim, shape);
    if (n < 0) return lube->error(vm, "that Array would be too big");
    void *data;
    LubeValue v = lube->new_foreign(vm, ArrayType, &data);
    Array *a = (Array *)data;
    a->buf = buffer_new((size_t)n * (size_t)ITEMSIZE[dtype], zero);
    if (!a->buf) {  /* LCOV_EXCL_START: out of memory */
        lube->release(v);
        return lube->error(vm, "not enough memory for an Array of %.0f elements", (double)n);
    }  /* LCOV_EXCL_STOP */
    a->data = a->buf->data;
    a->dtype = dtype;
    a->ndim = ndim;
    a->size = n;
    for (int d = 0; d < ndim; d++) a->shape[d] = shape[d];
    c_strides(a);
    *out = a;
    return v;
}

/* A new Array object viewing src's memory (its shape and strides to be set by the caller). */
static LubeValue arr_view(LubeVM *vm, const Array *src, Array **out) {
    void *data;
    LubeValue v = lube->new_foreign(vm, ArrayType, &data);
    Array *a = (Array *)data;
    *a = *src;
    a->buf->refs++;
    *out = a;
    return v;
}

static void set_size(Array *a) { a->size = shape_size(a->ndim, a->shape); }

/* ---------- elements, as doubles or as int64 ---------- */

static double load(const unsigned char *p, int t) {
    switch (t) {
    case ARR_F64: return *(const double *)p;
    case ARR_F32: return *(const float *)p;
    case ARR_I64: return (double)*(const int64_t *)p;
    case ARR_I32: return *(const int32_t *)p;
    default: return *p;  /* uint8, bool (0 or 1) */
    }
}

static int64_t to_i64(double x) {  /* (NaN and what does not fit: the smallest, as numpy gives) */
    return x >= -9223372036854775808.0 && x < 9223372036854775808.0 ? (int64_t)x : INT64_MIN;
}

static int64_t load_int(const unsigned char *p, int t) {  /* (t an integer dtype, or bool) */
    switch (t) {
    case ARR_I64: return *(const int64_t *)p;
    case ARR_I32: return *(const int32_t *)p;
    default: return *p;
    }
}

static void store(unsigned char *p, int t, double x) {
    switch (t) {
    case ARR_F64: *(double *)p = x; break;
    case ARR_F32: *(float *)p = (float)x; break;
    case ARR_I64: *(int64_t *)p = to_i64(x); break;
    case ARR_I32: *(int32_t *)p = (int32_t)(uint32_t)(uint64_t)to_i64(x); break;  /* (wraps, as numpy's) */
    case ARR_U8: *p = (unsigned char)(uint64_t)to_i64(x); break;
    default: *p = x != 0; break;
    }
}

static void store_int(unsigned char *p, int t, int64_t x) {  /* (likewise) */
    switch (t) {
    case ARR_I64: *(int64_t *)p = x; break;
    case ARR_I32: *(int32_t *)p = (int32_t)(uint32_t)(uint64_t)x; break;
    case ARR_U8: *p = (unsigned char)(uint64_t)x; break;
    default: *p = x != 0; break;
    }
}

/* The element at a C-order position. */
static unsigned char *elem_at(const Array *a, int64_t flat) {
    int64_t off = 0;
    for (int d = a->ndim - 1; d >= 0; d--) {
        off += (flat % a->shape[d]) * a->strides[d];
        flat /= a->shape[d];
    }
    return a->data + off * ITEMSIZE[a->dtype];
}

/* ======================================================================
 * Loops: over a shape, for up to 4 operands with strides of their own
 * (0 along a broadcast axis); axes that lie one after another in all of
 * them are merged, so the innermost runs are as long as they can be.
 * ==================================================================== */

typedef struct {
    int nd, nops;
    int64_t shape[ARR_MAXDIM];
    int64_t st[4][ARR_MAXDIM];
    unsigned char *base[4];
    int size[4];  /* each operand's item size */
} Loop;

/* One innermost run: n elements, at p[k] for each operand, st[k] elements apart. */
typedef void (*RunFn)(void *ctx, int64_t n, unsigned char *const *p, const int64_t *st);

/* A loop's shape and each operand's strides along it, before loop_make merges axes. */
typedef struct {
    int nd;
    int64_t shape[ARR_MAXDIM];
    int64_t st[4][ARR_MAXDIM];
} LoopSpec;

static void loop_make(Loop *L, const LoopSpec *s, int nops, unsigned char *const *base, const int *size) {
    L->nops = nops;
    L->nd = 0;
    for (int k = 0; k < nops; k++) {
        L->base[k] = base[k];
        L->size[k] = size[k];
    }
    for (int d = 0; d < s->nd; d++) {
        if (s->shape[d] == 1) continue;
        int merge = L->nd > 0;
        for (int k = 0; k < nops && merge; k++) merge = L->st[k][L->nd - 1] == s->st[k][d] * s->shape[d];
        if (merge) {  /* the previous axis steps over this whole one: one axis */
            L->shape[L->nd - 1] *= s->shape[d];
            for (int k = 0; k < nops; k++) L->st[k][L->nd - 1] = s->st[k][d];
            continue;
        }
        L->shape[L->nd] = s->shape[d];
        for (int k = 0; k < nops; k++) L->st[k][L->nd] = s->st[k][d];
        L->nd++;
    }
    if (L->nd == 0) {  /* one element */
        L->shape[0] = 1;
        for (int k = 0; k < nops; k++) L->st[k][0] = 0;
        L->nd = 1;
    }
}

/* How many innermost runs there are, and each one's length. */
static int64_t loop_runs(const Loop *L) {
    int64_t n = 1;
    for (int d = 0; d < L->nd - 1; d++) n *= L->shape[d];
    return n;
}

/* Runs from..to: runs when there are several, else positions in the one run. */
static void loop_part(const Loop *L, int64_t from, int64_t to, RunFn fn, void *ctx) {
    unsigned char *p[4];
    int64_t st[4];
    int in = L->nd - 1;
    for (int k = 0; k < L->nops; k++) st[k] = L->st[k][in];
    if (L->nd == 1) {
        for (int k = 0; k < L->nops; k++) p[k] = L->base[k] + from * st[k] * L->size[k];
        fn(ctx, to - from, p, st);
        return;
    }
    int64_t idx[ARR_MAXDIM], r = from;
    for (int d = in - 1; d >= 0; d--) {
        idx[d] = r % L->shape[d];
        r /= L->shape[d];
    }
    for (int64_t run = from; run < to; run++) {
        for (int k = 0; k < L->nops; k++) {
            int64_t off = 0;
            for (int d = 0; d < in; d++) off += idx[d] * L->st[k][d];
            p[k] = L->base[k] + off * L->size[k];
        }
        fn(ctx, L->shape[in], p, st);
        for (int d = in - 1; d >= 0; d--) {
            if (++idx[d] < L->shape[d]) break;
            idx[d] = 0;
        }
    }
}

typedef struct {
    const Loop *L;
    RunFn fn;
    void *ctx;
} LoopJob;

static void loop_piece(void *ctx, int64_t from, int64_t to) {
    LoopJob *j = (LoopJob *)ctx;
    loop_part(j->L, from, to, j->fn, j->ctx);
}

#define PARALLEL_WORK ((int64_t)1 << 17)  /* elements worth sharing out among threads (fewer: one thread) */

static void par_for(int64_t n, int64_t grain, void (*fn)(void *ctx, int64_t from, int64_t to), void *ctx);

/* Every run of the loop; on several threads when there is enough work: cost is what an element costs, next to
 * an addition's (exp's is some 30 times as much, and is worth sharing out sooner). */
static void loop_go(const Loop *L, RunFn fn, void *ctx, int cost) {
    int64_t runs = loop_runs(L), len = L->shape[L->nd - 1], work = PARALLEL_WORK / (cost > 0 ? cost : 1);
    LoopJob j = {L, fn, ctx};
    if (runs == 1) par_for(len, work, loop_piece, &j);
    else par_for(runs, work / (len ? len : 1) + 1, loop_piece, &j);
}

/* The shape two broadcast to (numpy's rule: from the last axis, equal or 1). 0 if they don't. */
static int broadcast(const Array *a, const Array *b, int *nd, int64_t *shape) {
    int n = a->ndim > b->ndim ? a->ndim : b->ndim;
    for (int i = 0; i < n; i++) {
        int da = a->ndim - n + i, db = b->ndim - n + i;
        int64_t x = da >= 0 ? a->shape[da] : 1, y = db >= 0 ? b->shape[db] : 1;
        if (x != y && x != 1 && y != 1) return 0;
        shape[i] = x == 1 ? y : x;
    }
    *nd = n;
    return 1;
}

/* a's strides along shape (nd axes, a broadcast to it): 0 where a has length 1 or no such axis. */
static void broadcast_strides(const Array *a, int nd, const int64_t *shape, int64_t *st) {
    for (int i = 0; i < nd; i++) {
        int d = a->ndim - nd + i;
        st[i] = d >= 0 && a->shape[d] == shape[i] && shape[i] != 1 ? a->strides[d] : 0;
    }
}

static void shape_text(char *buf, size_t size, int nd, const int64_t *shape) {
    size_t at = (size_t)snprintf(buf, size, "[");
    for (int d = 0; d < nd && at < size; d++) at += (size_t)snprintf(buf + at, size - at, "%s%.0f", d ? ", " : "", (double)shape[d]);
    if (at < size) snprintf(buf + at, size - at, "]");
}

static LubeValue broadcast_error(LubeVM *vm, const Array *a, const Array *b) {
    char x[200], y[200];
    shape_text(x, sizeof x, a->ndim, a->shape);
    shape_text(y, sizeof y, b->ndim, b->shape);
    return lube->error(vm, "shapes %s and %s do not broadcast together", x, y);
}

/* ---------- copying with conversion ---------- */

typedef struct {
    int from, to;  /* dtypes */
} CopyCtx;

static void copy_run(void *ctx, int64_t n, unsigned char *const *p, const int64_t *st) {
    const CopyCtx *c = (const CopyCtx *)ctx;
    unsigned char *d = p[0], *s = p[1];
    int dz = ITEMSIZE[c->to], sz = ITEMSIZE[c->from];
    if (c->from == c->to) {
        if (st[0] == 1 && st[1] == 1) {
            memcpy(d, s, (size_t)n * (size_t)dz);
        } else if (dz == 8) {
            for (int64_t i = 0; i < n; i++) ((uint64_t *)d)[i * st[0]] = ((const uint64_t *)s)[i * st[1]];
        } else if (dz == 4) {
            for (int64_t i = 0; i < n; i++) ((uint32_t *)d)[i * st[0]] = ((const uint32_t *)s)[i * st[1]];
        } else {
            for (int64_t i = 0; i < n; i++) d[i * st[0]] = s[i * st[1]];
        }
    } else if (c->to == ARR_F32 && c->from == ARR_F64 && st[0] == 1 && st[1] == 1) {
        for (int64_t i = 0; i < n; i++) ((float *)d)[i] = (float)((const double *)s)[i];
    } else if (c->to == ARR_F64 && c->from == ARR_F32 && st[0] == 1 && st[1] == 1) {
        for (int64_t i = 0; i < n; i++) ((double *)d)[i] = ((const float *)s)[i];
    } else if (st[0] == 1 && st[1] == 1 && c->to == ARR_BOOL) {  /* (the common ones, in a row: loops of their own) */
        switch (c->from) {
        case ARR_F64: for (int64_t i = 0; i < n; i++) d[i] = ((const double *)s)[i] != 0; break;
        case ARR_F32: for (int64_t i = 0; i < n; i++) d[i] = ((const float *)s)[i] != 0; break;
        case ARR_I64: for (int64_t i = 0; i < n; i++) d[i] = ((const int64_t *)s)[i] != 0; break;
        case ARR_I32: for (int64_t i = 0; i < n; i++) d[i] = ((const int32_t *)s)[i] != 0; break;
        default: for (int64_t i = 0; i < n; i++) d[i] = s[i] != 0; break;  /* uint8 */
        }
    } else if (st[0] == 1 && st[1] == 1 && c->to == ARR_F64 && !IS_FLOAT(c->from)) {
        switch (c->from) {
        case ARR_I64: for (int64_t i = 0; i < n; i++) ((double *)d)[i] = (double)((const int64_t *)s)[i]; break;
        case ARR_I32: for (int64_t i = 0; i < n; i++) ((double *)d)[i] = ((const int32_t *)s)[i]; break;
        default: for (int64_t i = 0; i < n; i++) ((double *)d)[i] = s[i]; break;  /* uint8, bool */
        }
    } else if (st[0] == 1 && st[1] == 1 && c->to == ARR_I64 && c->from == ARR_F64) {  /* (to_i64's, in a loop) */
        for (int64_t i = 0; i < n; i++) {
            double x = ((const double *)s)[i];
            ((int64_t *)d)[i] = x >= -9223372036854775808.0 && x < 9223372036854775808.0 ? (int64_t)x : INT64_MIN;
        }
    } else if (st[0] == 1 && st[1] == 1 && c->to == ARR_I64 && !IS_FLOAT(c->from)) {
        if (c->from == ARR_I32) for (int64_t i = 0; i < n; i++) ((int64_t *)d)[i] = ((const int32_t *)s)[i];
        else for (int64_t i = 0; i < n; i++) ((int64_t *)d)[i] = s[i];  /* uint8, bool */
    } else if (!IS_FLOAT(c->from) && !IS_FLOAT(c->to)) {  /* integers: exactly */
        for (int64_t i = 0; i < n; i++) store_int(d + i * st[0] * dz, c->to, load_int(s + i * st[1] * sz, c->from));
    } else {
        for (int64_t i = 0; i < n; i++) store(d + i * st[0] * dz, c->to, load(s + i * st[1] * sz, c->from));
    }
}

/* Copies src into dst (same shape, or src broadcasting to it), converting its dtype. */
static void copy_into(Array *dst, const Array *src) {
    LoopSpec s;
    s.nd = dst->ndim;
    for (int d = 0; d < dst->ndim; d++) {
        s.shape[d] = dst->shape[d];
        s.st[0][d] = dst->strides[d];
    }
    broadcast_strides(src, dst->ndim, dst->shape, s.st[1]);
    unsigned char *base[2] = {dst->data, src->data};
    int size[2] = {ITEMSIZE[dst->dtype], ITEMSIZE[src->dtype]};
    Loop L;
    loop_make(&L, &s, 2, base, size);
    CopyCtx c = {src->dtype, dst->dtype};
    loop_go(&L, copy_run, &c, 1);
}

/* A C-contiguous copy of a, of dtype (-1: a's own). */
static LubeValue arr_copy(LubeVM *vm, const Array *a, int dtype, Array **out) {
    Array *r;
    LubeValue v = arr_new(vm, dtype < 0 ? a->dtype : dtype, a->ndim, a->shape, &r, 0);
    if (out) *out = r;
    if (!r) return v;  /* LCOV_EXCL_LINE: (too big: a has the same size) */
    copy_into(r, a);
    return v;
}

/* x (an Array) as C-contiguous, of dtype (-1: its own): x itself, retained, when it already is. */
static LubeValue as_contiguous(LubeVM *vm, LubeValue x, int dtype, Array **out) {
    Array *a = arr_of(x);
    if (is_contiguous(a) && (dtype < 0 || dtype == a->dtype)) {
        *out = a;
        return lube->retain(x);
    }
    return arr_copy(vm, a, dtype, out);
}

/* Makes a's memory its own before it changes (copy-on-write): a copy when another Array views it, or when
 * some of its elements are one (a broadcast view's: stride 0). */
static void arr_own(Array *a) {
    int overlaps = 0;
    for (int d = 0; d < a->ndim; d++) overlaps |= a->strides[d] == 0 && a->shape[d] > 1;
    if (a->buf->refs == 1 && !overlaps) return;
    ArrBuffer *b = buffer_new((size_t)a->size * (size_t)ITEMSIZE[a->dtype], 0);
    if (!b) abort();  /* LCOV_EXCL_LINE: out of memory */
    Array copy = *a;
    copy.buf = b;
    copy.data = b->data;
    c_strides(&copy);
    copy_into(&copy, a);
    a->buf->refs--;
    *a = copy;
}

/* ======================================================================
 * From Lube's values
 * ==================================================================== */

/* A 0-dimensional Array holding one Number (or Bool). */
static LubeValue scalar_array(LubeVM *vm, double x, int dtype, Array **out) {
    Array *a;
    LubeValue v = arr_new(vm, dtype, 0, NULL, &a, 0);
    store(a->data, dtype, x);
    *out = a;
    return v;
}

/* The shape of nested lists (and Arrays inside them), from their first items; 0 with an error message. */
static int nested_shape(LubeValue x, int depth, int *nd, int64_t *shape, int *all_bool, const char **err, const char **what) {
    int k = lube->kind(x);
    Array *a = arr_of(x);
    if (a) {
        if (depth + a->ndim > ARR_MAXDIM) {
            *err = "too many dimensions";
            return 0;
        }
        for (int d = 0; d < a->ndim; d++) shape[depth + d] = a->shape[d];
        *nd = depth + a->ndim;
        if (a->dtype != ARR_BOOL) *all_bool = 0;
        return 1;
    }
    if (k == LUBE_LIST) {
        if (depth == ARR_MAXDIM) {
            *err = "too many dimensions";
            return 0;
        }
        size_t n = lube->list_len(x);
        shape[depth] = (int64_t)n;
        if (n == 0) {
            *nd = depth + 1;
            return 1;
        }
        return nested_shape(lube->list_get(x, 0), depth + 1, nd, shape, all_bool, err, what);
    }
    double s, e, st;
    int incl;
    if (lube->range(x, &s, &e, &st, &incl)) {
        if (depth == ARR_MAXDIM) {
            *err = "too many dimensions";
            return 0;
        }
        double n = st > 0 ? ceil((e - s) / st) + (incl && fmod(e - s, st) == 0) : 0;
        if (!(n >= 0) || n > (double)MAX_ELEMENTS) {
            *err = "a range too long for an Array";
            return 0;
        }
        shape[depth] = (int64_t)n;
        *nd = depth + 1;
        *all_bool = 0;
        return 1;
    }
    if (k == LUBE_NUMBER || k == LUBE_BOOL) {
        if (k == LUBE_NUMBER) *all_bool = 0;
        *nd = depth;
        return 1;
    }
    *err = "an Array holds Numbers and Bools, not";
    *what = lube->a_type(x);
    return 0;
}

/* Fills a (C-contiguous) from nested lists of that shape, from position *at; 0 with an error message. */
static int fill_nested(Array *a, LubeValue x, int depth, int64_t *at, const char **err, const char **what) {
    Array *sub = arr_of(x);
    if (sub) {
        if (sub->ndim != a->ndim - depth) {
            *err = "lists of different shapes";
            return 0;
        }
        for (int d = 0; d < sub->ndim; d++)
            if (sub->shape[d] != a->shape[depth + d]) {
                *err = "lists of different shapes";
                return 0;
            }
        Array dst = *a;
        dst.data = a->data + *at * ITEMSIZE[a->dtype];
        dst.ndim = sub->ndim;
        for (int d = 0; d < sub->ndim; d++) dst.shape[d] = sub->shape[d];
        c_strides(&dst);
        copy_into(&dst, sub);
        *at += sub->size;
        return 1;
    }
    int k = lube->kind(x);
    double s, e, st;
    int incl;
    if (depth == a->ndim) {
        if (k != LUBE_NUMBER && k != LUBE_BOOL) {
            int list = k == LUBE_LIST || lube->range(x, &s, &e, &st, &incl);
            *err = list ? "lists of different shapes" : "an Array holds Numbers and Bools, not";
            if (!list) *what = lube->a_type(x);
            return 0;
        }
        store(a->data + (*at)++ * ITEMSIZE[a->dtype], a->dtype, k == LUBE_BOOL ? lube->truthy(x) : lube->number(x));
        return 1;
    }
    if (lube->range(x, &s, &e, &st, &incl)) {
        if (depth != a->ndim - 1) {
            *err = "lists of different shapes";
            return 0;
        }
        for (int64_t i = 0; i < a->shape[depth]; i++) store(a->data + (*at)++ * ITEMSIZE[a->dtype], a->dtype, s + (double)i * st);
        return 1;
    }
    if (k != LUBE_LIST || (int64_t)lube->list_len(x) != a->shape[depth]) {
        *err = "lists of different shapes";
        return 0;
    }
    for (size_t i = 0; i < lube->list_len(x); i++)
        if (!fill_nested(a, lube->list_get(x, i), depth + 1, at, err, what)) return 0;
    return 1;
}

/* array(x, dtype): an Array of anything that makes one; dtype -1: float64, or bool for Bools, or x's own. */
static LubeValue arr_from(LubeVM *vm, LubeValue x, int dtype, Array **out) {
    *out = NULL;
    Array *a = arr_of(x);
    if (a) {
        if (dtype < 0 || dtype == a->dtype) {
            *out = a;
            return lube->retain(x);
        }
        return arr_copy(vm, a, dtype, out);
    }
    size_t blen;
    const unsigned char *bytes = lube->bytes(x, &blen);
    if (bytes) {  /* Bytes: uint8 */
        int64_t n = (int64_t)blen;
        Array *r;
        LubeValue v = arr_new(vm, ARR_U8, 1, &n, &r, 0);
        if (blen) memcpy(r->data, bytes, blen);
        if (dtype >= 0 && dtype != ARR_U8) {
            LubeValue c = arr_copy(vm, r, dtype, out);
            lube->release(v);
            return c;
        }
        *out = r;
        return v;
    }
    int nd = 0, all_bool = 1;
    int64_t shape[ARR_MAXDIM];
    const char *err = NULL, *what = "";
    if (!nested_shape(x, 0, &nd, shape, &all_bool, &err, &what))
        return lube->error(vm, "array(): %s%s%s", err, *what ? " " : "", what);
    if (nd > 0 && shape[0] == 0 && lube->kind(x) == LUBE_LIST) all_bool = 0;  /* ([]: float64) */
    Array *r;
    LubeValue v = arr_new(vm, dtype >= 0 ? dtype : all_bool ? ARR_BOOL : ARR_F64, nd, shape, &r, 0);
    if (!r) return v;
    int64_t at = 0;
    if (!fill_nested(r, x, 0, &at, &err, &what)) {
        lube->release(v);
        return lube->error(vm, "array(): %s%s%s", err, *what ? " " : "", what);
    }
    *out = r;
    return v;
}

/* The Lube value of one element: a Number, or a Bool for a bool Array. */
static LubeValue elem_value(const unsigned char *p, int dtype) {
    return dtype == ARR_BOOL ? lube->boolean(*p) : lube->number_value(load(p, dtype));
}

/* a as nested lists (a 0-dimensional one: its Number). */
static LubeValue to_list(LubeVM *vm, const Array *a, int depth, const unsigned char *p) {
    if (depth == a->ndim) return elem_value(p, a->dtype);
    LubeValue l = lube->new_list(vm);
    for (int64_t i = 0; i < a->shape[depth]; i++)
        lube->list_push(l, to_list(vm, a, depth + 1, p + i * a->strides[depth] * ITEMSIZE[a->dtype]));
    return l;
}

/* ======================================================================
 * Printing: [[1, 2], [3, 4]], one row a line; long ones shortened with ...
 * ==================================================================== */

typedef struct {
    char *s;
    size_t len, cap;
} Text;

static void text_put(Text *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        char *grown = (char *)realloc(t->s, t->cap);
        if (!grown) abort();  /* LCOV_EXCL_LINE: out of memory */
        t->s = grown;
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = '\0';
}

static void text_str(Text *t, const char *s) { text_put(t, s, strlen(s)); }

/* A number as Lube prints it; a float32 with the fewest digits that give it back. */
static void number_text(char *buf, size_t size, double x, int dtype) {
    if (dtype == ARR_F32 && isfinite(x)) {
        for (int digits = 1; digits <= 9; digits++) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.*g", digits, x);
            double back = strtod(tmp, NULL);
            if ((float)back == (float)x) {
                x = back;
                break;
            }
        }
    }
    lube->format_number(x, buf, size);
}

#define SHOW_EDGE 3        /* a shortened axis shows this many at each end */
#define SHOW_ALL 1000      /* Arrays of more elements than this are shortened */

static void print_axis(Text *t, const Array *a, int depth, const unsigned char *p, int shorten) {
    if (depth == a->ndim) {
        char num[64];
        if (a->dtype == ARR_BOOL) snprintf(num, sizeof num, "%s", *p ? "true" : "false");
        else number_text(num, sizeof num, load(p, a->dtype), a->dtype);
        text_str(t, num);
        return;
    }
    text_str(t, "[");
    int64_t n = a->shape[depth];
    for (int64_t i = 0; i < n; i++) {
        if (shorten && n > 2 * SHOW_EDGE && i == SHOW_EDGE) {
            text_str(t, depth == a->ndim - 1 ? "..., " : "...,\n");
            if (depth < a->ndim - 1)
                for (int s = 0; s <= depth; s++) text_str(t, " ");
            i = n - SHOW_EDGE;
        }
        print_axis(t, a, depth + 1, p + i * a->strides[depth] * ITEMSIZE[a->dtype], shorten);
        if (i < n - 1) {
            text_str(t, ",");
            if (depth == a->ndim - 1) {
                text_str(t, " ");
            } else {
                for (int s = depth + 1; s < a->ndim - 1; s++) text_str(t, "\n");  /* (a blank line between blocks) */
                text_str(t, "\n");
                for (int s = 0; s <= depth; s++) text_str(t, " ");
            }
        }
    }
    text_str(t, "]");
}

static int arr_describe(const void *data, char *buf, size_t size) {
    const Array *a = (const Array *)data;
    Text t = {NULL, 0, 0};
    text_str(&t, "");
    print_axis(&t, a, 0, a->data, a->size > SHOW_ALL);
    int n = snprintf(buf, size, "%s", t.s);
    free(t.s);
    return n;
}
