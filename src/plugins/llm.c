/*
 * llm.c - the plugin llm: language models (llama.cpp's GGUF files), run on
 * the CPU.
 *
 *   import llm
 *   let m = llm.load("tinyllama.Q4_K_M.gguf")   a Model (or llm.Model(path)): options threads, context, mmap
 *   m.architecture, m.name, m.vocab_size...     what the file says of it (m.metadata: all of it)
 *   m.tokenize("Hello"), m.detokenize(tokens)   text to token ids and back
 *   m.forward(tokens)                           the next token's logits (an Array), after the tokens before
 *   m.generate("Once upon a time", {max_tokens: 50, temperature: 0})
 *                                               text that follows the prompt (on_token: as it comes)
 *   m.reset(), m.tokens, m.tensor(name), m.tensors
 *
 * Models of the llama family: llama, mistral, qwen2, qwen3, phi3, gemma;
 * weights in F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q4_K, Q5_K and
 * Q6_K (the common downloads: Q4_K_M, Q8_0...). The file is mapped into
 * memory and the weights stay as they are: the products quantize the
 * vector and multiply integers, on array's threads. The tokenizers are the
 * model's own (SentencePiece's or GPT-2's byte-level BPE), and sampling
 * draws from array's random numbers (array.seed(n), or the seed option).
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* mmap, fseeko under -std=c99 */
#endif
#define _FILE_OFFSET_BITS 64     /* files past 2 GB on 32-bit systems */
#include "array.h"

#ifdef __clang__  /* (no fused multiply-adds: the same results on every system; gcc's -std=c99 makes none) */
#pragma STDC FP_CONTRACT OFF
#endif

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef __int64 FilePos;
#define file_seek _fseeki64
#define file_tell _ftelli64
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
typedef off_t FilePos;
#define file_seek fseeko
#define file_tell ftello
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define FNAME (__func__ + 2)  /* "m_generate" -> "generate" */

static const LubeAPI *lube;
static const ArrayExports *arr;

#include "llm/gguf.c"
#include "llm/quant.c"
#include "llm/unicode.c"
#include "llm/vocab.c"
#include "llm/model.c"

static LubeType *ModelType;

typedef struct {
    Model *m;
} Box;

static Model *model_of(LubeValue v) { return ((Box *)lube->foreign(v, ModelType))->m; }

static void box_release(void *data) { model_free(((Box *)data)->m); }

static int box_describe(const void *data, char *buf, size_t size) {
    const Model *m = ((const Box *)data)->m;
    return snprintf(buf, size, "Model(%s, %d layer%s, %d tokens, %s)", m->arch, m->n_layer, m->n_layer == 1 ? "" : "s",
                    m->n_vocab, m->path);
}

/* ======================================================================
 * Arguments and options
 * ==================================================================== */

/* Checks that argv[i] is an object of options whose names are all in `names` ("a, b, c"). */
static int check_options(LubeVM *vm, const char *fname, const LubeValue *argv, int i, const char *names, LubeValue *err) {
    if (lube->kind(argv[i]) != LUBE_OBJECT) {
        *err = lube->error(vm, "%s() expects an object of options as argument %d, got %s", fname, i + 1, lube->a_type(argv[i]));
        return 0;
    }
    LubeValue keys = lube->object_keys(vm, argv[i]);
    int ok = 1;
    for (size_t k = 0; k < lube->list_len(keys) && ok; k++) {
        size_t n;
        const char *key = lube->str(lube->list_get(keys, k), &n);
        const char *p = key ? strstr(names, key) : NULL;
        while (p && ((p != names && p[-1] != ' ') || (p[n] != ',' && p[n] != '\0'))) p = strstr(p + 1, key);
        if (!p) {
            *err = lube->error(vm, "%s(): there is no option '%s' (the options: %s)", fname, key ? key : "?", names);
            ok = 0;
        }
    }
    lube->release(keys);
    return ok;
}

/* Option `key` as a Number from lo to hi (whole if asked): 1 if given (*out), 0 if not, -1 after an error. */
static int opt_number(LubeVM *vm, const char *fname, LubeValue opts, const char *key, double lo, double hi, int whole,
                      double *out, LubeValue *err) {
    LubeValue v;
    if (lube->kind(opts) != LUBE_OBJECT || !lube->object_get(vm, opts, key, &v)) return 0;
    double d = lube->number(v);
    if (lube->kind(v) != LUBE_NUMBER || !(d >= lo && d <= hi) || (whole && d != floor(d))) {
        char a[32], b[32];
        lube->format_number(lo, a, sizeof a);
        lube->format_number(hi, b, sizeof b);
        *err = lube->error(vm, "%s(): the option %s must be a %sNumber from %s to %s, not %s", fname, key,
                           whole ? "whole " : "", a, b, lube->a_type(v));
        if (lube->kind(v) == LUBE_NUMBER) {
            char c[32];
            lube->format_number(d, c, sizeof c);
            lube->release(*err);
            *err = lube->error(vm, "%s(): the option %s must be a %sNumber from %s to %s, not %s", fname, key,
                               whole ? "whole " : "", a, b, c);
        }
        return -1;
    }
    *out = d;
    return 1;
}

/* Option `key` as a Bool: 1 if given (*out), 0 if not, -1 after an error. */
static int opt_bool(LubeVM *vm, const char *fname, LubeValue opts, const char *key, int *out, LubeValue *err) {
    LubeValue v;
    if (lube->kind(opts) != LUBE_OBJECT || !lube->object_get(vm, opts, key, &v)) return 0;
    if (lube->kind(v) != LUBE_BOOL) {
        *err = lube->error(vm, "%s(): the option %s must be true or false, not %s", fname, key, lube->a_type(v));
        return -1;
    }
    *out = lube->truthy(v);
    return 1;
}

/* Token ids from v (a List of them, or one): 1, or 0 with *err. */
static int tokens_arg(LubeVM *vm, const char *fname, const Model *m, LubeValue v, int i, Ids *out, LubeValue *err) {
    int list = lube->kind(v) == LUBE_LIST;
    size_t n = list ? lube->list_len(v) : 1;
    if (!list && lube->kind(v) != LUBE_NUMBER) {
        *err = lube->error(vm, "%s() expects a List of token ids as argument %d, got %s", fname, i + 1, lube->a_type(v));
        return 0;
    }
    for (size_t k = 0; k < n; k++) {
        LubeValue x = list ? lube->list_get(v, k) : v;
        double d = lube->number(x);
        if (lube->kind(x) != LUBE_NUMBER || d != floor(d) || d < 0 || d >= m->n_vocab) {
            char num[32] = "";
            if (lube->kind(x) == LUBE_NUMBER) lube->format_number(d, num, sizeof num);
            *err = lube->error(vm, "%s(): %s is not a token id (they go from 0 to %d)", fname,
                               lube->kind(x) == LUBE_NUMBER ? num : lube->a_type(x), m->n_vocab - 1);
            return 0;
        }
        ids_push(out, (int)d);
    }
    return 1;
}

static LubeValue busy_error(LubeVM *vm, const char *fname) {
    return lube->error(vm, "%s(): the model is generating (on_token cannot change it)", fname);
}

/* ======================================================================
 * Loading
 * ==================================================================== */

/* load(path, options?): a Model. Options: threads (0: array's), context (tokens it holds), mmap. */
FN(f_load) {
    LubeValue err;
    if (lube->kind(argv[0]) != LUBE_STR)
        return lube->error(vm, "%s() expects a str (a GGUF file's path) as argument 1, got %s", FNAME, lube->a_type(argv[0]));
    const char *path = lube->str(argv[0], NULL);
    LubeValue opts = argc > 1 ? argv[1] : lube->nothing();
    double threads = 0, context = 0;
    int use_map = 1;
    if (argc > 1 && !check_options(vm, FNAME, argv, 1, "threads, context, mmap", &err)) return err;
    if (opt_number(vm, FNAME, opts, "threads", 0, 256, 1, &threads, &err) < 0 ||
        opt_number(vm, FNAME, opts, "context", 1, 1 << 24, 1, &context, &err) < 0 ||
        opt_bool(vm, FNAME, opts, "mmap", &use_map, &err) < 0)
        return err;
    Model *m = (Model *)alloc_or_die(sizeof(Model));
    char msg[600], why[400] = "";
    snprintf(m->path, sizeof m->path, "%s", path);
    if (!gguf_open(&m->g, path, use_map, msg, sizeof msg)) {
        model_free(m);
        return lube->error(vm, "%s(): %s", FNAME, msg);
    }
    if (!model_init(m, (int)context, why, sizeof why)) {
        model_free(m);
        return lube->error(vm, "%s(): '%s' %s", FNAME, path, why);
    }
    m->threads = (int)threads;
    void *data;
    LubeValue v = lube->new_foreign(vm, ModelType, &data);
    ((Box *)data)->m = m;
    return v;
}

/* ======================================================================
 * Properties
 * ==================================================================== */

#define PROP_NUM(fname, expr)                                                                                  \
    FN(fname) {                                                                                                \
        (void)vm;                                                                                              \
        (void)argc;                                                                                            \
        const Model *m = model_of(argv[0]);                                                                    \
        return lube->number_value((double)(expr));                                                             \
    }
PROP_NUM(p_vocab_size, m->n_vocab)
PROP_NUM(p_context_length, m->n_ctx_train)
PROP_NUM(p_context, m->n_ctx)
PROP_NUM(p_layers, m->n_layer)
PROP_NUM(p_embedding_length, m->n_embd)
PROP_NUM(p_heads, m->n_head)
PROP_NUM(p_kv_heads, m->n_kv)
PROP_NUM(p_feed_forward_length, m->n_ff)
PROP_NUM(p_parameters, m->n_params)
PROP_NUM(p_size, m->weight_bytes)
PROP_NUM(p_bos, m->vocab.bos)
PROP_NUM(p_eos, m->vocab.eos)

FN(p_architecture) {
    (void)argc;
    const Model *m = model_of(argv[0]);
    return lube->new_str(vm, m->arch, strlen(m->arch));
}

FN(p_name) {
    (void)argc;
    Str s = {"", 0};
    kv_str(&model_of(argv[0])->g, "general.name", &s);
    return lube->new_str(vm, s.s, s.n);
}

/* the weights' format: general.file_type's name, else the type most of the weights have */
FN(p_quantization) {
    (void)argc;
    static const char *const FTYPES[] = {"F32", "F16", "Q4_0", "Q4_1", NULL, NULL, NULL, "Q8_0", "Q5_0", "Q5_1",
                                         "Q2_K", "Q3_K_S", "Q3_K_M", "Q3_K_L", "Q4_K_S", "Q4_K_M", "Q5_K_S",
                                         "Q5_K_M", "Q6_K"};
    const Model *m = model_of(argv[0]);
    double ft = -1;
    if (kv_num(&m->g, "general.file_type", &ft) && ft >= 0 && ft < 19 && FTYPES[(int)ft])
        return lube->new_str(vm, FTYPES[(int)ft], strlen(FTYPES[(int)ft]));
    double count[T_NTYPES] = {0};
    int best = T_F32;
    for (int64_t i = 0; i < m->g.n_tensors; i++) {
        const Tensor *t = &m->g.tensors[i];
        if (t->ndim < 2) continue;
        count[t->type] += (double)t->ne[0] * (double)t->ne[1] * (double)t->ne[2] * (double)t->ne[3];
        if (count[t->type] > count[best]) best = t->type;
    }
    return lube->new_str(vm, type_name(best), strlen(type_name(best)));
}

FN(p_tokens) {
    (void)argc;
    const Model *m = model_of(argv[0]);
    LubeValue l = lube->new_list(vm);
    for (int i = 0; i < m->n_past; i++) lube->list_push(l, lube->number_value(m->past[i]));
    return l;
}

FN(p_metadata) {
    (void)argc;
    return gguf_metadata(vm, &model_of(argv[0])->g);
}

static LubeValue shape_of(LubeVM *vm, const Tensor *t) {
    LubeValue l = lube->new_list(vm);
    for (int d = t->ndim - 1; d >= 0; d--) lube->list_push(l, lube->number_value((double)t->ne[d]));
    return l;
}

/* every tensor: {name, type, shape} (shape as numpy has it: rows first) */
FN(p_tensors) {
    (void)argc;
    const Model *m = model_of(argv[0]);
    LubeValue l = lube->new_list(vm);
    for (int64_t i = 0; i < m->g.n_tensors; i++) {
        const Tensor *t = &m->g.tensors[i];
        LubeValue o = lube->new_object(vm);
        lube->object_set(vm, o, "name", lube->new_str(vm, t->name.s, t->name.n));
        lube->object_set(vm, o, "type", lube->new_str(vm, type_name(t->type), strlen(type_name(t->type))));
        lube->object_set(vm, o, "shape", shape_of(vm, t));
        lube->list_push(l, o);
    }
    return l;
}

/* ======================================================================
 * Methods
 * ==================================================================== */

/* m.tensor(name): its numbers as a float32 Array */
FN(m_tensor) {
    (void)argc;
    const Model *m = model_of(argv[0]);
    if (lube->kind(argv[1]) != LUBE_STR)
        return lube->error(vm, "%s() expects a str (a tensor's name) as argument 2, got %s", FNAME, lube->a_type(argv[1]));
    const char *name = lube->str(argv[1], NULL);
    const Tensor *t = tensor_find(&m->g, name);
    if (!t) return lube->error(vm, "%s(): the model has no tensor '%s'", FNAME, name);
    if (!type_usable(t->type)) return lube->error(vm, "%s(): '%s' is of type %s, which llm cannot read", FNAME, name, type_name(t->type));
    int64_t shape[4];
    for (int d = 0; d < t->ndim; d++) shape[d] = t->ne[t->ndim - 1 - d];
    void *data;
    LubeValue a = arr->make(vm, ARR_F32, t->ndim, shape, &data);
    int64_t rows = t->ne[1] * t->ne[2] * t->ne[3], rb = type_row_bytes(t->type, t->ne[0]);
    for (int64_t r = 0; r < rows; r++) dequant_row(t->type, t->data + r * rb, (float *)data + r * t->ne[0], t->ne[0]);
    return a;
}

/* m.tokenize(text, {bos, eos, special}): token ids. bos and eos: as the model has it; special: false
 * (true: special tokens' text in it, such as <|im_start|>, become those tokens). */
FN(m_tokenize) {
    const Model *m = model_of(argv[0]);
    LubeValue err;
    if (lube->kind(argv[1]) != LUBE_STR) return lube->error(vm, "%s() expects a str as argument 2, got %s", FNAME, lube->a_type(argv[1]));
    int bos = m->vocab.add_bos, eos = m->vocab.add_eos, special = 0;
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing();
    if (argc > 2 && !check_options(vm, FNAME, argv, 2, "bos, eos, special", &err)) return err;
    if (opt_bool(vm, FNAME, opts, "bos", &bos, &err) < 0 || opt_bool(vm, FNAME, opts, "eos", &eos, &err) < 0 ||
        opt_bool(vm, FNAME, opts, "special", &special, &err) < 0)
        return err;
    size_t n;
    const char *s = lube->str(argv[1], &n);
    Ids ids = {NULL, 0, 0};
    tokenize(&m->vocab, s, n, bos, eos, special, &ids);
    LubeValue l = lube->new_list(vm);
    for (int i = 0; i < ids.n; i++) lube->list_push(l, lube->number_value(ids.ids[i]));
    free(ids.ids);
    return l;
}

/* m.detokenize(tokens, {special}): their text (special: with control tokens' text, such as </s>) */
FN(m_detokenize) {
    const Model *m = model_of(argv[0]);
    LubeValue err;
    int special = 0;
    LubeValue opts = argc > 2 ? argv[2] : lube->nothing();
    if (argc > 2 && !check_options(vm, FNAME, argv, 2, "special", &err)) return err;
    if (opt_bool(vm, FNAME, opts, "special", &special, &err) < 0) return err;
    Ids ids = {NULL, 0, 0};
    if (!tokens_arg(vm, FNAME, m, argv[1], 1, &ids, &err)) {
        free(ids.ids);
        return err;
    }
    Text t = {NULL, 0, 0};
    detokenize(&m->vocab, ids.ids, ids.n, special, &t);
    LubeValue s = lube->new_str(vm, t.s ? t.s : "", t.n);
    free(t.s);
    free(ids.ids);
    return s;
}

static LubeValue logits_array(LubeVM *vm, const Model *m) {
    int64_t n = m->n_vocab;
    void *data;
    LubeValue a = arr->make(vm, ARR_F32, 1, &n, &data);
    memcpy(data, m->logits, sizeof(float) * (size_t)n);
    return a;
}

/* m.forward(tokens): the tokens through the model after those it has seen (the cache): the logits for the
 * token after the last, as a float32 Array. */
FN(m_forward) {
    (void)argc;
    Model *m = model_of(argv[0]);
    LubeValue err;
    if (m->busy) return busy_error(vm, FNAME);
    Ids ids = {NULL, 0, 0};
    if (!tokens_arg(vm, FNAME, m, argv[1], 1, &ids, &err)) {
        free(ids.ids);
        return err;
    }
    char msg[200];
    if (!ids.n) {
        free(ids.ids);
        return lube->error(vm, "%s(): no tokens", FNAME);
    }
    int ok = model_forward(m, ids.ids, ids.n, msg, sizeof msg);
    free(ids.ids);
    return ok ? logits_array(vm, m) : lube->error(vm, "%s(): %s", FNAME, msg);
}

/* m.reset(): empties the cache */
FN(m_reset) {
    (void)argc;
    Model *m = model_of(argv[0]);
    if (m->busy) return busy_error(vm, FNAME);
    m->n_past = 0;
    return lube->nothing();
}

/* ---------- generating ---------- */

typedef struct {
    double temperature, top_p, repeat_penalty;
    int top_k;
} Sampling;

typedef struct {
    int id;
    float logit;
} Cand;

static int by_logit(const void *a, const void *b) {
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->logit != y->logit) return x->logit < y->logit ? 1 : -1;
    return x->id - y->id;
}

/* The next token from the logits (changed by the repeat penalty). cand has room for every token. */
static int sample(Model *m, const Sampling *s, Cand *cand) {
    float *logits = m->logits;
    int n = m->n_vocab;
    if (s->repeat_penalty != 1) {  /* each token of the last 64, once: less likely */
        int from = m->n_past > 64 ? m->n_past - 64 : 0;
        for (int i = from; i < m->n_past; i++) {
            int t = m->past[i], seen = 0;
            for (int j = from; j < i && !seen; j++) seen = m->past[j] == t;
            if (!seen) logits[t] = logits[t] > 0 ? (float)(logits[t] / s->repeat_penalty) : (float)(logits[t] * s->repeat_penalty);
        }
    }
    if (s->temperature == 0) {
        int best = 0;
        for (int i = 1; i < n; i++)
            if (logits[i] > logits[best]) best = i;
        return best;
    }
    int k = s->top_k > 0 && s->top_k < n ? s->top_k : n, nc = 0;
    if (k == n) {
        for (int i = 0; i < n; i++) cand[i] = (Cand){i, logits[i]};
        qsort(cand, (size_t)n, sizeof(Cand), by_logit);
        nc = n;
    } else {
        for (int i = 0; i < n; i++) {  /* the k best, best first */
            if (nc == k && !(logits[i] > cand[k - 1].logit)) continue;
            int p = nc < k ? nc++ : k - 1;
            while (p > 0 && cand[p - 1].logit < logits[i]) {
                cand[p] = cand[p - 1];
                p--;
            }
            cand[p] = (Cand){i, logits[i]};
        }
    }
    double total = 0, *prob = (double *)alloc_or_die(sizeof(double) * (size_t)nc);
    for (int i = 0; i < nc; i++) total += prob[i] = exp(((double)cand[i].logit - cand[0].logit) / s->temperature);
    int keep = nc;
    if (s->top_p < 1) {  /* the fewest best tokens that make up top_p of it */
        double sum = 0;
        for (keep = 0; keep < nc && (keep == 0 || sum < s->top_p * total); keep++) sum += prob[keep];
        total = sum;
    }
    double u;
    arr->fill_random(&u, ARR_F64, 1, 0, 0.0, 1.0);
    u *= total;
    int pick = keep - 1;
    for (int i = 0; i < keep - 1; i++) {
        if (u < prob[i]) {
            pick = i;
            break;
        }
        u -= prob[i];
    }
    free(prob);
    return cand[pick].id;
}

typedef struct {
    LubeVM *vm;
    LubeValue on_token;  /* a function, or nothing */
    Text out;            /* the text so far */
    size_t sent;         /* how much of it on_token has had */
    int stop;            /* 1: on_token said to stop */
    LubeValue error;     /* on_token's error */
    int failed;
} Stream;

/* Gives on_token the text up to `upto`: 0 if it raised an error. */
static int emit(Stream *st, size_t upto) {
    if (upto <= st->sent || lube->kind(st->on_token) == LUBE_NOTHING) {
        if (upto > st->sent) st->sent = upto;
        return 1;
    }
    LubeValue piece = lube->new_str(st->vm, st->out.s + st->sent, upto - st->sent);
    LubeValue r = lube->call(st->vm, st->on_token, 1, &piece);
    lube->release(piece);
    st->sent = upto;
    if (lube->failed(r)) {
        st->error = r;
        st->failed = 1;
        return 0;
    }
    if (lube->kind(r) == LUBE_BOOL && !lube->truthy(r)) st->stop = 1;
    lube->release(r);
    return 1;
}

/* m.generate(prompt, options?): the text the model writes after the prompt (a str, or a List of token ids). Options:
 * max_tokens (256), temperature (0.8; 0: always the likeliest token), top_k (40; 0: all), top_p (0.95),
 * repeat_penalty (1: none), seed (else array's generator as it is), stop (a str or a List of them: it ends
 * before one), on_token (a function, given each piece of text as it comes; false from it stops). What the
 * cache holds of the prompt already is not computed again. */
FN(m_generate) {
    Model *m = model_of(argv[0]);
    LubeValue err, opts = argc > 2 ? argv[2] : lube->nothing();
    if (m->busy) return busy_error(vm, FNAME);
    if (argc > 2 && !check_options(vm, FNAME, argv, 2,
                                   "max_tokens, temperature, top_k, top_p, repeat_penalty, seed, stop, on_token", &err))
        return err;
    double max_tokens = 256, top_k = 40, seed = -1;
    Sampling s = {0.8, 0.95, 1, 40};
    if (opt_number(vm, FNAME, opts, "max_tokens", 0, 1 << 30, 1, &max_tokens, &err) < 0 ||
        opt_number(vm, FNAME, opts, "temperature", 0, 1e6, 0, &s.temperature, &err) < 0 ||
        opt_number(vm, FNAME, opts, "top_k", 0, 1 << 30, 1, &top_k, &err) < 0 ||
        opt_number(vm, FNAME, opts, "top_p", 0, 1, 0, &s.top_p, &err) < 0 ||
        opt_number(vm, FNAME, opts, "repeat_penalty", 0.01, 100, 0, &s.repeat_penalty, &err) < 0 ||
        opt_number(vm, FNAME, opts, "seed", 0, 4294967295.0, 1, &seed, &err) < 0)
        return err;
    s.top_k = (int)top_k;
    LubeValue on_token = lube->nothing(), stop = lube->nothing();
    if (lube->kind(opts) == LUBE_OBJECT) {
        lube->object_get(vm, opts, "on_token", &on_token);
        lube->object_get(vm, opts, "stop", &stop);
    }
    int n_stop = lube->kind(stop) == LUBE_LIST ? (int)lube->list_len(stop) : lube->kind(stop) == LUBE_STR;
    for (int i = 0; i < n_stop; i++) {
        LubeValue x = lube->kind(stop) == LUBE_LIST ? lube->list_get(stop, (size_t)i) : stop;
        size_t n;
        if (lube->kind(x) != LUBE_STR || (lube->str(x, &n), n == 0))
            return lube->error(vm, "%s(): the option stop must be a str or a List of them (not empty), not %s", FNAME,
                               lube->kind(x) == LUBE_STR ? "\"\"" : lube->a_type(x));
    }
    if (lube->kind(stop) != LUBE_NOTHING && lube->kind(stop) != LUBE_LIST && lube->kind(stop) != LUBE_STR)
        return lube->error(vm, "%s(): the option stop must be a str or a List of them (not empty), not %s", FNAME, lube->a_type(stop));

    Ids prompt = {NULL, 0, 0};
    if (lube->kind(argv[1]) == LUBE_STR) {
        size_t n;
        const char *text = lube->str(argv[1], &n);
        tokenize(&m->vocab, text, n, m->vocab.add_bos, 0, 1, &prompt);
    } else if (lube->kind(argv[1]) == LUBE_LIST) {
        if (!tokens_arg(vm, FNAME, m, argv[1], 1, &prompt, &err)) {
            free(prompt.ids);
            return err;
        }
    } else {
        return lube->error(vm, "%s() expects a str or a List of token ids as argument 2, got %s", FNAME, lube->a_type(argv[1]));
    }
    if (prompt.n == 0 || prompt.n > m->n_ctx) {
        int n = prompt.n;
        free(prompt.ids);
        return n ? lube->error(vm, "%s(): the prompt (%d tokens) does not fit in the context (%d)", FNAME, n, m->n_ctx)
                 : lube->error(vm, "%s(): the prompt has no tokens", FNAME);
    }
    int common = 0;  /* (what the cache has of the prompt already) */
    while (common < m->n_past && common < prompt.n && m->past[common] == prompt.ids[common]) common++;
    if (common == prompt.n) common--;
    m->n_past = common;
    char msg[200];
    int ok = model_forward(m, prompt.ids + common, prompt.n - common, msg, sizeof msg);  /* (it fits: checked above) */
    free(prompt.ids);
    if (!ok) return lube->error(vm, "%s(): %s", FNAME, msg);  /* LCOV_EXCL_LINE: out of memory */
    if (seed >= 0) arr->seed((uint32_t)seed);

    Stream st = {vm, on_token, {NULL, 0, 0}, 0, 0, lube->nothing(), 0};
    Cand *cand = (Cand *)alloc_or_die(sizeof(Cand) * (size_t)m->n_vocab);
    m->busy = 1;
    for (int i = 0; i < (int)max_tokens; i++) {
        int tok = sample(m, &s, cand);
        if (vocab_is_eog(&m->vocab, tok)) break;
        size_t before = st.out.n, upto;
        token_piece(&m->vocab, tok, 0, &st.out);
        int stopped = 0;
        size_t cut = st.out.n;
        for (int k = 0; k < n_stop; k++) {  /* a stop string: the text ends before the first one */
            size_t n;
            const char *ss = lube->str(lube->kind(stop) == LUBE_LIST ? lube->list_get(stop, (size_t)k) : stop, &n);
            for (size_t p = before >= n - 1 ? before - (n - 1) : 0; p + n <= st.out.n; p++)
                if (!memcmp(st.out.s + p, ss, n)) {
                    if (p < cut) cut = p;
                    stopped = 1;
                    break;
                }
        }
        st.out.n = cut;
        upto = st.out.n;
        for (int k = 0; k < n_stop && !stopped; k++) {  /* (held back: what may be the start of one) */
            size_t n;
            const char *ss = lube->str(lube->kind(stop) == LUBE_LIST ? lube->list_get(stop, (size_t)k) : stop, &n);
            for (size_t len = n - 1 < st.out.n ? n - 1 : st.out.n; len > 0; len--)
                if (st.out.n - len < upto && !memcmp(st.out.s + st.out.n - len, ss, len)) {
                    upto = st.out.n - len;
                    break;
                }
        }
        if (!stopped) upto -= utf8_unfinished((const unsigned char *)st.out.s, upto);
        if (!emit(&st, upto) || stopped || st.stop || i + 1 == (int)max_tokens || m->n_past >= m->n_ctx) break;
        if (!model_forward(m, &tok, 1, msg, sizeof msg)) break;
    }
    if (!st.failed) emit(&st, st.out.n);
    m->busy = 0;
    free(cand);
    LubeValue result = st.failed ? st.error : lube->new_str(vm, st.out.s ? st.out.s : "", st.out.n);
    free(st.out.s);
    return result;
}

LUBE_PLUGIN(llm) {
    lube = api;
    if (api->version < 2) return -1;
    arr = (const ArrayExports *)api->require(plugin, "array");
    if (!arr || arr->version < ARRAY_EXPORTS) return -1;  /* LCOV_EXCL_LINE: array comes with Lube */
    special_lock = api->lock_new();
    bpe_tables();
    api->function(plugin, "load", f_load, 1, 2);
    ModelType = api->type(plugin, "Model", sizeof(Box), box_release, box_describe);
    api->constructor(ModelType, f_load, 1, 2);
    api->method(ModelType, "tokenize", m_tokenize, 2, 3);
    api->method(ModelType, "detokenize", m_detokenize, 2, 3);
    api->method(ModelType, "forward", m_forward, 2, 2);
    api->method(ModelType, "generate", m_generate, 2, 3);
    api->method(ModelType, "reset", m_reset, 1, 1);
    api->method(ModelType, "tensor", m_tensor, 2, 2);
    static const struct {
        const char *name;
        LubeFn fn;
    } props[] = {
        {"architecture", p_architecture}, {"name", p_name}, {"vocab_size", p_vocab_size},
        {"context_length", p_context_length}, {"context", p_context}, {"layers", p_layers},
        {"embedding_length", p_embedding_length}, {"heads", p_heads}, {"kv_heads", p_kv_heads},
        {"feed_forward_length", p_feed_forward_length}, {"parameters", p_parameters}, {"size", p_size},
        {"quantization", p_quantization}, {"bos", p_bos}, {"eos", p_eos}, {"tokens", p_tokens},
        {"metadata", p_metadata}, {"tensors", p_tensors},
    };
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) api->property(ModelType, props[i].name, props[i].fn);
    return LUBE_PLUGIN_API;
}
