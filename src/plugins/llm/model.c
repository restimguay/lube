/*
 * llm/model.c - a transformer of the llama family, run on the CPU: its
 * sizes from the metadata, its weights where they lie in the file, a cache
 * of the attention's keys and values (it grows as it fills), and the
 * forward pass, a batch of tokens at a time. (Part of llm.c.)
 *
 * The families differ in small ways: qwen2 adds biases to q, k and v;
 * qwen3 normalizes q and k per head; phi3 keeps q, k, v in one matrix and
 * the feed-forward's gate and up in another; gemma scales the embeddings
 * and uses GELU; and all but llama and mistral turn q and k (RoPE) by
 * halves rather than by neighbours.
 */

#define PAR_PIECE 65536       /* multiply-adds a piece of it has at least */
#define PAR_MIN_WORK 4194304   /* multiply-adds a job needs before it is shared among threads */

typedef struct {
    int type;
    int64_t rows, cols;
    const unsigned char *data;
    int64_t row_bytes;
} Weight;

typedef struct {
    Weight q, k, v, qkv, o, gate, up, down;  /* qkv: q, k and v in one (phi3); up without gate: gate and up in one */
    float *attn_norm, *ffn_norm, *q_norm, *k_norm, *bq, *bk, *bv;
} Layer;

static const struct {
    const char *name;
    int neox, gelu, scale_embd;
} ARCHS[] = {
    {"llama", 0, 0, 0}, {"mistral", 0, 0, 0}, {"qwen2", 1, 0, 0}, {"qwen3", 1, 0, 0}, {"phi3", 1, 0, 0},
    {"gemma", 1, 1, 1},
};

typedef struct Model {
    Gguf g;
    Vocab vocab;
    char path[512];
    const char *arch;
    int neox, gelu;
    int n_vocab, n_embd, n_layer, n_head, n_kv, hd, n_ff, n_ctx_train, n_ctx, rope_dim, q_dim, kv_dim;
    float eps, embd_scale, rope_scale;
    double *inv_freq;        /* RoPE: each pair's angle per position */
    Weight tok_embd, output;
    float *out_norm;
    Layer *layers;
    float **vecs;            /* the norms and biases, as floats (malloc'd: the layers point into them) */
    int n_vecs;
    int64_t n_params;
    uint64_t weight_bytes;
    int threads;             /* 0: array's threads; 1: none; more: that many parts */
    int busy;                /* generating: on_token may not change the model */
    /* the cache */
    int n_past, kv_cap;
    int *past;               /* the tokens in it */
    float **kc, **vc;        /* per layer: kv_cap positions of kv_dim keys, and values */
    /* room for a batch */
    float *x, *xb, *q, *k, *v, *qkv, *att, *hb, *hb2, *logits, *rope_cs;
    Act xa;                  /* the input of a product, quantized */
} Model;

static void model_free(Model *m) {
    gguf_free(&m->g);
    vocab_free(&m->vocab);
    for (int i = 0; i < m->n_vecs; i++) free(m->vecs[i]);
    free(m->vecs);
    for (int l = 0; m->kc && l < m->n_layer; l++) {
        free(m->kc[l]);
        free(m->vc[l]);
    }
    free(m->kc);
    free(m->vc);
    free(m->layers);
    free(m->inv_freq);
    free(m->past);
    free(m->x);
    free(m->xb);
    free(m->q);
    free(m->k);
    free(m->v);
    free(m->qkv);
    free(m->att);
    free(m->hb);
    free(m->hb2);
    free(m->logits);
    free(m->rope_cs);
    free(m->xa.q);
    free(m->xa.d);
    free(m->xa.s);
    free(m);
}

static void *alloc_or_die(size_t n) {
    void *p = calloc(n ? n : 1, 1);
    if (!p) abort();  /* LCOV_EXCL_LINE: out of memory */
    return p;
}

/* ---------- reading the model ---------- */

typedef struct {
    Model *m;
    char *err;
    size_t errsize;
} Loader;

static int fail(Loader *ld, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ld->err, ld->errsize, fmt, ap);
    va_end(ap);
    return 0;
}

/* metadata "<arch>.key" as a whole number: 1 if there (or fallback >= 0 is used), else 0 with an error */
static int hparam(Loader *ld, const char *key, int fallback, int lo, int *out) {
    char full[128];
    double d = 0;
    snprintf(full, sizeof full, "%s.%s", ld->m->arch, key);
    if (!kv_num(&ld->m->g, full, &d)) {
        const Kv *kv = kv_find(&ld->m->g, full);
        if (kv) return fail(ld, "has %s as %s (llm takes one number for every layer)", full, kv->type == GV_ARR ? "a list" : "text");
        if (fallback < 0) return fail(ld, "has no %s", full);
        d = fallback;
    }
    if (d != floor(d) || d < lo || d > 1e8) return fail(ld, "has %s = %g, which llm cannot use", full, d);
    *out = (int)d;
    return 1;
}

static double fparam(Model *m, const char *key, double fallback) {
    char full[128];
    double d = 0;
    snprintf(full, sizeof full, "%s.%s", m->arch, key);
    return kv_num(&m->g, full, &d) ? d : fallback;
}

static char *tname(char *buf, int layer, const char *what) {
    if (layer < 0) snprintf(buf, 96, "%s", what);
    else snprintf(buf, 96, "blk.%d.%s", layer, what);
    return buf;
}

/* A matrix of rows x cols: 1, or 0 (an error unless it is optional and missing: then w->data is NULL). */
static int bind_matrix(Loader *ld, Weight *w, int layer, const char *what, int64_t cols, int64_t rows, int optional) {
    char name[96];
    const Tensor *t = tensor_find(&ld->m->g, tname(name, layer, what));
    memset(w, 0, sizeof *w);
    if (!t) return optional ? 0 : fail(ld, "has no tensor '%s'", name);
    if (t->ne[0] != cols || t->ne[1] != rows || t->ne[2] * t->ne[3] != 1)
        return fail(ld, "has tensor '%s' of shape [%.0f, %.0f], not [%.0f, %.0f]", name, (double)t->ne[0], (double)t->ne[1],
                    (double)cols, (double)rows);
    if (!type_usable(t->type)) return fail(ld, "has tensor '%s' of type %s, which llm cannot use", name, type_name(t->type));
    if (t->type == T_F32 && ((uintptr_t)t->data & 3)) return fail(ld, "has tensor '%s' at a place not a multiple of 4", name);
    w->type = t->type;
    w->rows = rows;
    w->cols = cols;
    w->data = t->data;
    w->row_bytes = type_row_bytes(t->type, cols);
    ld->m->n_params += rows * cols;
    ld->m->weight_bytes += t->nbytes;
    return 1;
}

/* A vector of n numbers, as floats: 1 (*out set), or 0 (an error unless it is optional and missing: then NULL). */
static int bind_vector(Loader *ld, float **out, int layer, const char *what, int64_t n, int optional) {
    char name[96];
    Model *m = ld->m;
    const Tensor *t = tensor_find(&m->g, tname(name, layer, what));
    *out = NULL;
    if (!t) return optional ? 1 : fail(ld, "has no tensor '%s'", name);
    if (t->ne[0] != n || t->ne[1] * t->ne[2] * t->ne[3] != 1)
        return fail(ld, "has tensor '%s' of %.0f numbers, not %.0f", name, (double)(t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3]), (double)n);
    if (!type_usable(t->type)) return fail(ld, "has tensor '%s' of type %s, which llm cannot use", name, type_name(t->type));
    *out = (float *)alloc_or_die(sizeof(float) * (size_t)n);
    dequant_row(t->type, t->data, *out, n);
    m->vecs[m->n_vecs++] = *out;
    m->n_params += n;
    m->weight_bytes += t->nbytes;
    return 1;
}

/* Reads the model's sizes and finds its weights: 1, or 0 with a message in err. */
static int model_init(Model *m, int n_ctx, char *err, size_t errsize) {
    Loader ld = {m, err, errsize};
    Str arch = {"", 0}, scaling = {"", 0};
    if (!kv_str(&m->g, "general.architecture", &arch)) return fail(&ld, "has no general.architecture");
    for (size_t i = 0; i < sizeof ARCHS / sizeof ARCHS[0]; i++)
        if (str_is(arch, ARCHS[i].name)) {
            m->arch = ARCHS[i].name;
            m->neox = ARCHS[i].neox;
            m->gelu = ARCHS[i].gelu;
            m->embd_scale = ARCHS[i].scale_embd ? -1 : 1;
        }
    if (!m->arch)
        return fail(&ld, "is a model of architecture '%.*s' (llm runs llama, mistral, qwen2, qwen3, phi3 and gemma)",
                    (int)arch.n, arch.s);
    if (!vocab_load(&m->vocab, &m->g, err, errsize)) return 0;
    if (!hparam(&ld, "embedding_length", -1, 1, &m->n_embd) || !hparam(&ld, "block_count", -1, 1, &m->n_layer) ||
        !hparam(&ld, "feed_forward_length", -1, 1, &m->n_ff) || !hparam(&ld, "attention.head_count", -1, 1, &m->n_head) ||
        !hparam(&ld, "attention.head_count_kv", m->n_head, 1, &m->n_kv) ||
        !hparam(&ld, "context_length", 2048, 1, &m->n_ctx_train) ||
        !hparam(&ld, "attention.key_length", m->n_embd / m->n_head, 2, &m->hd) ||
        !hparam(&ld, "rope.dimension_count", m->hd, 2, &m->rope_dim))
        return 0;
    int hdv;
    if (!hparam(&ld, "attention.value_length", m->hd, 1, &hdv)) return 0;
    if (hdv != m->hd) return fail(&ld, "has keys and values of different lengths (%d and %d)", m->hd, hdv);
    if (m->n_head % m->n_kv) return fail(&ld, "has %d heads, not a multiple of its %d key-value heads", m->n_head, m->n_kv);
    if (m->hd % 2 || m->rope_dim % 2 || m->rope_dim > m->hd)
        return fail(&ld, "has heads of %d numbers turned %d at a time, which llm cannot use", m->hd, m->rope_dim);
    m->n_vocab = m->vocab.n;
    m->q_dim = m->n_head * m->hd;
    m->kv_dim = m->n_kv * m->hd;
    m->eps = (float)fparam(m, "attention.layer_norm_rms_epsilon", 1e-5);
    m->n_ctx = n_ctx > 0 ? n_ctx : m->n_ctx_train;
    if (m->embd_scale < 0) m->embd_scale = (float)sqrt((double)m->n_embd);
    char key[128];
    snprintf(key, sizeof key, "%s.rope.scaling.type", m->arch);
    m->rope_scale = 1;
    if (kv_str(&m->g, key, &scaling) && str_is(scaling, "linear")) m->rope_scale = (float)fparam(m, "rope.scaling.factor", 1);
    if (!(m->rope_scale > 0)) return fail(&ld, "has a RoPE scaling factor of %g", (double)m->rope_scale);

    m->layers = (Layer *)alloc_or_die(sizeof(Layer) * (size_t)m->n_layer);
    m->vecs = (float **)alloc_or_die(sizeof(float *) * (size_t)(m->n_layer * 7 + 2));
    int64_t E = m->n_embd, V = m->n_vocab;
    if (!bind_matrix(&ld, &m->tok_embd, -1, "token_embd.weight", E, V, 0)) return 0;
    if (!bind_matrix(&ld, &m->output, -1, "output.weight", E, V, 1)) {
        if (ld.err[0]) return 0;
        m->output = m->tok_embd;  /* (the embeddings, tied) */
    }
    if (!bind_vector(&ld, &m->out_norm, -1, "output_norm.weight", E, 0)) return 0;
    for (int l = 0; l < m->n_layer; l++) {
        Layer *L = &m->layers[l];
        if (!bind_vector(&ld, &L->attn_norm, l, "attn_norm.weight", E, 0) ||
            !bind_vector(&ld, &L->ffn_norm, l, "ffn_norm.weight", E, 0) ||
            !bind_vector(&ld, &L->q_norm, l, "attn_q_norm.weight", m->hd, 1) ||
            !bind_vector(&ld, &L->k_norm, l, "attn_k_norm.weight", m->hd, 1) ||
            !bind_vector(&ld, &L->bq, l, "attn_q.bias", m->q_dim, 1) || !bind_vector(&ld, &L->bk, l, "attn_k.bias", m->kv_dim, 1) ||
            !bind_vector(&ld, &L->bv, l, "attn_v.bias", m->kv_dim, 1))
            return 0;
        if (!bind_matrix(&ld, &L->qkv, l, "attn_qkv.weight", E, m->q_dim + 2 * m->kv_dim, 1)) {
            if (ld.err[0] || !bind_matrix(&ld, &L->q, l, "attn_q.weight", E, m->q_dim, 0) ||
                !bind_matrix(&ld, &L->k, l, "attn_k.weight", E, m->kv_dim, 0) ||
                !bind_matrix(&ld, &L->v, l, "attn_v.weight", E, m->kv_dim, 0))
                return 0;
        }
        if (!bind_matrix(&ld, &L->o, l, "attn_output.weight", m->q_dim, E, 0) ||
            !bind_matrix(&ld, &L->down, l, "ffn_down.weight", m->n_ff, E, 0))
            return 0;
        int fused = !bind_matrix(&ld, &L->gate, l, "ffn_gate.weight", E, m->n_ff, 1);
        if (ld.err[0] || !bind_matrix(&ld, &L->up, l, "ffn_up.weight", E, (fused ? 2 : 1) * (int64_t)m->n_ff, 0)) return 0;
    }
    float *factors = NULL;
    if (!bind_vector(&ld, &factors, -1, "rope_freqs.weight", m->rope_dim / 2, 1)) return 0;
    double base = fparam(m, "rope.freq_base", 10000);
    m->inv_freq = (double *)alloc_or_die(sizeof(double) * (size_t)(m->rope_dim / 2));
    for (int i = 0; i < m->rope_dim / 2; i++)
        m->inv_freq[i] = pow(base, -2.0 * i / m->rope_dim) / m->rope_scale / (factors ? factors[i] : 1.0);

    int B = MAX_BATCH, big = m->q_dim > E ? m->q_dim : (int)E;
    if (m->n_ff > big) big = m->n_ff;
    m->x = (float *)alloc_or_die(sizeof(float) * (size_t)(B * E));
    m->xb = (float *)alloc_or_die(sizeof(float) * (size_t)B * (size_t)big);
    m->q = (float *)alloc_or_die(sizeof(float) * (size_t)(B * m->q_dim));
    m->k = (float *)alloc_or_die(sizeof(float) * (size_t)(B * m->kv_dim));
    m->v = (float *)alloc_or_die(sizeof(float) * (size_t)(B * m->kv_dim));
    m->qkv = (float *)alloc_or_die(sizeof(float) * (size_t)B * (size_t)(m->q_dim + 2 * m->kv_dim));
    m->att = (float *)alloc_or_die(sizeof(float) * (size_t)(B * m->q_dim));
    m->hb = (float *)alloc_or_die(sizeof(float) * (size_t)B * 2 * (size_t)m->n_ff);
    m->hb2 = (float *)alloc_or_die(sizeof(float) * (size_t)B * (size_t)m->n_ff);
    m->logits = (float *)alloc_or_die(sizeof(float) * (size_t)V);
    m->rope_cs = (float *)alloc_or_die(sizeof(float) * (size_t)(B * m->rope_dim));
    m->xa.q = (int8_t *)alloc_or_die((size_t)B * (size_t)blocks4(big) * 32);
    m->xa.d = (float *)alloc_or_die(sizeof(float) * (size_t)B * (size_t)blocks4(big));
    m->xa.s = (int32_t *)alloc_or_die(sizeof(int32_t) * (size_t)B * (size_t)blocks4(big));
    m->kc = (float **)alloc_or_die(sizeof(float *) * (size_t)m->n_layer);
    m->vc = (float **)alloc_or_die(sizeof(float *) * (size_t)m->n_layer);
    return 1;
}

/* ---------- sharing work among threads ---------- */

typedef void (*Job)(void *ctx, int64_t from, int64_t to);

typedef struct {
    Job fn;
    void *ctx;
    int64_t n, parts;
} Parts;

static void run_part(void *c, int64_t from, int64_t to) {
    Parts *p = (Parts *)c;
    for (int64_t i = from; i < to; i++) p->fn(p->ctx, p->n * i / p->parts, p->n * (i + 1) / p->parts);
}

/* fn over 0..n, each item `work` multiply-adds: on this thread when it is less than min_work, else on several. */
static void run(const Model *m, int64_t n, int64_t work, int64_t min_work, Job fn, void *ctx) {
    if (m->threads == 1 || n < 2 || n * work < min_work) {
        fn(ctx, 0, n);
    } else if (m->threads > 1) {
        Parts p = {fn, ctx, n, m->threads < n ? m->threads : n};
        arr->parallel(p.parts, 1, run_part, &p);
    } else {
        int64_t grain = PAR_PIECE / work;
        arr->parallel(n, grain < 1 ? 1 : grain, fn, ctx);
    }
}

/* ---------- matrix products ---------- */

typedef struct {
    const Weight *w;
    const float *bias;
    float *y;        /* row r of token b goes to y[b * ystride + r] */
    int64_t ystride;
} Out;

typedef struct {
    Out out[3];      /* up to three products of the same input, as one job */
    int n_out, B;
    const float *x;  /* the input, B rows of `cols` floats */
    Act xa;          /* and quantized: rows qs numbers and ds blocks apart */
    int64_t cols, qs, ds;
} MatJob;

static void mat_rows(void *ctx, int64_t from, int64_t to) {
    const MatJob *j = (const MatJob *)ctx;
    int64_t first = 0;
    float y[MAX_BATCH];
    for (int o = 0; o < j->n_out; o++) {
        const Out *out = &j->out[o];
        const Weight *w = out->w;
        int64_t lo = from > first ? from - first : 0, hi = to - first < w->rows ? to - first : w->rows;
        for (int64_t r = lo; r < hi; r++) {
            row_dots(w->type, w->data + r * w->row_bytes, j->cols, j->x, j->cols, j->xa, j->qs, j->ds, j->B, y);
            for (int b = 0; b < j->B; b++) out->y[b * out->ystride + r] = out->bias ? y[b] + out->bias[r] : y[b];
        }
        first += w->rows;
    }
}

/* y = W x for up to three matrices of the same input x (B rows of cols), quantizing x first if one needs it. */
static void matmul(Model *m, const float *x, int B, int n_out, const Out *outs) {
    MatJob j;
    int quantized = 0;
    int64_t rows = 0;
    j.n_out = n_out;
    j.B = B;
    j.x = x;
    j.xa = m->xa;
    j.cols = outs[0].w->cols;
    j.ds = blocks4(j.cols);
    j.qs = 32 * j.ds;
    for (int o = 0; o < n_out; o++) {
        j.out[o] = outs[o];
        rows += outs[o].w->rows;
        quantized |= !type_is_float(outs[o].w->type);
    }
    for (int b = 0; quantized && b < B; b++) {
        Act a = {m->xa.q + b * j.qs, m->xa.d + b * j.ds, m->xa.s + b * j.ds};
        quantize_act(x + b * j.cols, j.cols, a);
    }
    run(m, rows, j.cols * B, PAR_MIN_WORK, mat_rows, &j);
}

static Out out_of(const Weight *w, const float *bias, float *y, int64_t ystride) {
    Out o = {w, bias, y, ystride};
    return o;
}

/* ---------- the other steps ---------- */

static void rms_norm(float *out, const float *x, const float *w, int n, float eps) {
    double ss = 0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * x[i];
    float scale = (float)(1.0 / sqrt(ss / n + eps));
    for (int i = 0; i < n; i++) out[i] = x[i] * scale * w[i];
}

/* Turns each head's first rope_dim numbers by the angles of its position (cs: cos, sin pairs). */
static void rope(float *x, int heads, int hd, int rope_dim, const float *cs, int neox) {
    int half_dim = rope_dim / 2;
    for (int h = 0; h < heads; h++, x += hd) {
        for (int i = 0; i < half_dim; i++) {
            int a = neox ? i : 2 * i, b = neox ? i + half_dim : 2 * i + 1;
            float c = cs[2 * i], s = cs[2 * i + 1], x0 = x[a], x1 = x[b];
            x[a] = x0 * c - x1 * s;
            x[b] = x0 * s + x1 * c;
        }
    }
}

typedef struct {
    Model *m;
    int layer, B, pos0;
} AttnJob;

/* Attention for items from..to: item = token of the batch * heads + head. */
static void attn_items(void *ctx, int64_t from, int64_t to) {
    const AttnJob *j = (const AttnJob *)ctx;
    const Model *m = j->m;
    int hd = m->hd, group = m->n_head / m->n_kv;
    float *score = (float *)malloc(sizeof(float) * (size_t)(j->pos0 + j->B));
    if (!score) abort();  /* LCOV_EXCL_LINE: out of memory */
    float scale = (float)(1.0 / sqrt((double)hd));
    for (int64_t item = from; item < to; item++) {
        int b = (int)(item / m->n_head), h = (int)(item % m->n_head), kvh = h / group, n = j->pos0 + b + 1;
        const float *q = m->q + (int64_t)b * m->q_dim + (int64_t)h * hd;
        const float *K = m->kc[j->layer] + (int64_t)kvh * hd, *V = m->vc[j->layer] + (int64_t)kvh * hd;
        float top = -INFINITY;
        for (int t = 0; t < n; t++) {
            score[t] = vec_dot(q, K + (int64_t)t * m->kv_dim, hd) * scale;
            if (score[t] > top) top = score[t];
        }
        double sum = 0;
        for (int t = 0; t < n; t++) {
            score[t] = expf(score[t] - top);
            sum += score[t];
        }
        float inv = (float)(1.0 / sum), *out = m->att + (int64_t)b * m->q_dim + (int64_t)h * hd;
        for (int i = 0; i < hd; i++) out[i] = 0;
        for (int t = 0; t < n; t++) {
            const float *v = V + (int64_t)t * m->kv_dim;
            float a = score[t] * inv;
            for (int i = 0; i < hd; i++) out[i] += a * v[i];
        }
    }
    free(score);
}

/* Room in the cache for `need` positions: 0 if there is no memory for it. */
static int kv_reserve(Model *m, int need) {
    if (need <= m->kv_cap) return 1;
    int cap = m->kv_cap ? m->kv_cap : 16;
    while (cap < need) cap = cap > m->n_ctx / 2 ? m->n_ctx : cap * 2;
    size_t bytes = sizeof(float) * (size_t)cap * (size_t)m->kv_dim;
    for (int l = 0; l < m->n_layer; l++) {
        float *k = (float *)realloc(m->kc[l], bytes);
        if (k) m->kc[l] = k;
        float *v = (float *)realloc(m->vc[l], bytes);
        if (v) m->vc[l] = v;
        if (!k || !v) return 0;  /* LCOV_EXCL_LINE: out of memory (the cache stays as it was) */
    }
    int *past = (int *)realloc(m->past, sizeof(int) * (size_t)cap);
    if (!past) return 0;  /* LCOV_EXCL_LINE: out of memory */
    m->past = past;
    m->kv_cap = cap;
    return 1;
}

static float silu(float x) { return x / (1.0f + expf(-x)); }

static float gelu(float x) { return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x))); }

/* B tokens (at positions n_past...) through the model: the last one's logits too if `last`. */
static void run_batch(Model *m, const int *tokens, int B, int last) {
    int E = m->n_embd, pos0 = m->n_past, F = m->n_ff;
    for (int b = 0; b < B; b++) {
        float *x = m->x + (int64_t)b * E;
        dequant_row(m->tok_embd.type, m->tok_embd.data + tokens[b] * m->tok_embd.row_bytes, x, E);
        if (m->embd_scale != 1)
            for (int i = 0; i < E; i++) x[i] *= m->embd_scale;
        for (int i = 0; i < m->rope_dim / 2; i++) {
            double angle = (double)(pos0 + b) * m->inv_freq[i];
            m->rope_cs[b * m->rope_dim + 2 * i] = (float)cos(angle);
            m->rope_cs[b * m->rope_dim + 2 * i + 1] = (float)sin(angle);
        }
    }
    for (int l = 0; l < m->n_layer; l++) {
        const Layer *L = &m->layers[l];
        for (int b = 0; b < B; b++) rms_norm(m->xb + (int64_t)b * E, m->x + (int64_t)b * E, L->attn_norm, E, m->eps);
        if (L->qkv.data) {
            int64_t n = m->q_dim + 2 * m->kv_dim;
            Out o = out_of(&L->qkv, NULL, m->qkv, n);
            matmul(m, m->xb, B, 1, &o);
            for (int b = 0; b < B; b++) {
                memcpy(m->q + (int64_t)b * m->q_dim, m->qkv + b * n, sizeof(float) * (size_t)m->q_dim);
                memcpy(m->k + (int64_t)b * m->kv_dim, m->qkv + b * n + m->q_dim, sizeof(float) * (size_t)m->kv_dim);
                memcpy(m->v + (int64_t)b * m->kv_dim, m->qkv + b * n + m->q_dim + m->kv_dim, sizeof(float) * (size_t)m->kv_dim);
            }
        } else {
            Out o[3] = {out_of(&L->q, L->bq, m->q, m->q_dim), out_of(&L->k, L->bk, m->k, m->kv_dim),
                        out_of(&L->v, L->bv, m->v, m->kv_dim)};
            matmul(m, m->xb, B, 3, o);
        }
        for (int b = 0; b < B; b++) {
            float *q = m->q + (int64_t)b * m->q_dim, *k = m->k + (int64_t)b * m->kv_dim;
            if (L->q_norm) {
                for (int h = 0; h < m->n_head; h++) rms_norm(q + h * m->hd, q + h * m->hd, L->q_norm, m->hd, m->eps);
                for (int h = 0; h < m->n_kv; h++) rms_norm(k + h * m->hd, k + h * m->hd, L->k_norm, m->hd, m->eps);
            }
            rope(q, m->n_head, m->hd, m->rope_dim, m->rope_cs + b * m->rope_dim, m->neox);
            rope(k, m->n_kv, m->hd, m->rope_dim, m->rope_cs + b * m->rope_dim, m->neox);
            memcpy(m->kc[l] + (int64_t)(pos0 + b) * m->kv_dim, k, sizeof(float) * (size_t)m->kv_dim);
            memcpy(m->vc[l] + (int64_t)(pos0 + b) * m->kv_dim, m->v + (int64_t)b * m->kv_dim, sizeof(float) * (size_t)m->kv_dim);
        }
        AttnJob aj = {m, l, B, pos0};
        run(m, (int64_t)B * m->n_head, (int64_t)(pos0 + B) * m->hd * 2, PAR_MIN_WORK / 2, attn_items, &aj);
        Out o = out_of(&L->o, NULL, m->xb, E);
        matmul(m, m->att, B, 1, &o);
        for (int64_t i = 0; i < (int64_t)B * E; i++) m->x[i] += m->xb[i];

        for (int b = 0; b < B; b++) rms_norm(m->xb + (int64_t)b * E, m->x + (int64_t)b * E, L->ffn_norm, E, m->eps);
        if (L->gate.data) {
            Out gu[2] = {out_of(&L->gate, NULL, m->hb, F), out_of(&L->up, NULL, m->hb2, F)};
            matmul(m, m->xb, B, 2, gu);
        } else {  /* (gate and up in one matrix: its first half is the gate) */
            Out gu = out_of(&L->up, NULL, m->hb, 2 * (int64_t)F);
            matmul(m, m->xb, B, 1, &gu);
            for (int b = 0; b < B; b++) {
                memmove(m->hb + (int64_t)b * F, m->hb + (int64_t)b * 2 * F, sizeof(float) * (size_t)F);
                memcpy(m->hb2 + (int64_t)b * F, m->hb + (int64_t)b * 2 * F + F, sizeof(float) * (size_t)F);
            }
        }
        for (int64_t i = 0; i < (int64_t)B * F; i++) m->hb[i] = (m->gelu ? gelu(m->hb[i]) : silu(m->hb[i])) * m->hb2[i];
        o = out_of(&L->down, NULL, m->xb, E);
        matmul(m, m->hb, B, 1, &o);
        for (int64_t i = 0; i < (int64_t)B * E; i++) m->x[i] += m->xb[i];
    }
    for (int b = 0; b < B; b++) m->past[pos0 + b] = tokens[b];
    m->n_past += B;
    if (last) {
        rms_norm(m->xb, m->x + (int64_t)(B - 1) * E, m->out_norm, E, m->eps);
        Out o = out_of(&m->output, NULL, m->logits, m->n_vocab);
        matmul(m, m->xb, 1, 1, &o);
    }
}

/* Tokens through the model after those in the cache; m->logits gets the last one's. 0 with a message in err. */
static int model_forward(Model *m, const int *tokens, int n, char *err, size_t errsize) {
    if (m->n_past + n > m->n_ctx) {
        snprintf(err, errsize, "the context is full: %d tokens and %d more do not fit in %d (reset() empties it)",
                 m->n_past, n, m->n_ctx);
        return 0;
    }
    if (!kv_reserve(m, m->n_past + n)) {  /* LCOV_EXCL_START: out of memory */
        snprintf(err, errsize, "no memory for a cache of %d tokens", m->n_past + n);
        return 0;
    }  /* LCOV_EXCL_STOP */
    for (int done = 0; done < n;) {
        int B = n - done < MAX_BATCH ? n - done : MAX_BATCH;
        run_batch(m, tokens + done, B, done + B == n);
        done += B;
    }
    return 1;
}
