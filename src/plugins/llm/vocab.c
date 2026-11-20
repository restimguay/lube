/*
 * llm/vocab.c - a model's tokenizer, from its GGUF metadata, as llama.cpp
 * has it:
 *   - SentencePiece's (llama, mistral, phi3): spaces become ▁ (and the text
 *     gets one in front), pairs of pieces merge best score first, and what
 *     no token covers falls back to tokens of its bytes, <0x0A>...
 *   - GPT-2's byte-level BPE (llama 3, qwen2): the text splits into words by
 *     the model's rules (a regex in Python's tokenizers, written out here),
 *     each byte becomes a character, and pairs merge in the merges' order.
 * Special tokens (<s>, <|im_start|>...) in the text are found first. (Part
 * of llm.c.)
 */

enum { TOK_NORMAL = 1, TOK_UNKNOWN = 2, TOK_CONTROL = 3, TOK_USER = 4, TOK_UNUSED = 5, TOK_BYTE = 6 };
enum { VOCAB_SPM, VOCAB_BPE };
enum { PRE_GPT2, PRE_LLAMA3, PRE_QWEN2, PRE_SMOLLM };

/* text -> index, into an array of Str (the later of two the same wins, as in llama.cpp) */
typedef struct {
    int32_t *slot;  /* index + 1, or 0 */
    uint32_t mask;
    const Str *keys;
} Hash;

static uint32_t hash_of(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void hash_build(Hash *h, const Str *keys, int n) {
    uint32_t cap = 16;
    while (cap < (uint32_t)n * 2) cap *= 2;
    h->slot = (int32_t *)calloc(cap, sizeof(int32_t));
    if (!h->slot) abort();  /* LCOV_EXCL_LINE: out of memory */
    h->mask = cap - 1;
    h->keys = keys;
    for (int i = 0; i < n; i++) {
        uint32_t at = hash_of(keys[i].s, keys[i].n) & h->mask;
        while (h->slot[at]) {
            const Str *k = &keys[h->slot[at] - 1];
            if (k->n == keys[i].n && !memcmp(k->s, keys[i].s, k->n)) break;
            at = (at + 1) & h->mask;
        }
        h->slot[at] = i + 1;
    }
}

static int hash_find(const Hash *h, const char *s, size_t n) {
    for (uint32_t at = hash_of(s, n) & h->mask; h->slot[at]; at = (at + 1) & h->mask) {
        const Str *k = &h->keys[h->slot[at] - 1];
        if (k->n == n && !memcmp(k->s, s, n)) return h->slot[at] - 1;
    }
    return -1;
}

/* a growing list of token ids */
typedef struct {
    int *ids;
    int n, cap;
} Ids;

static void ids_push(Ids *v, int id) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->ids = (int *)realloc(v->ids, sizeof(int) * (size_t)v->cap);
        if (!v->ids) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    v->ids[v->n++] = id;
}

/* a growing run of bytes */
typedef struct {
    char *s;
    size_t n, cap;
} Text;

static void text_add(Text *t, const void *p, size_t n) {
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 64;
        while (cap < t->n + n + 1) cap *= 2;
        t->s = (char *)realloc(t->s, cap);
        if (!t->s) abort();  /* LCOV_EXCL_LINE: out of memory */
        t->cap = cap;
    }
    memcpy(t->s + t->n, p, n);
    t->n += n;
    t->s[t->n] = '\0';
}

typedef struct {
    int kind;                       /* VOCAB_SPM or VOCAB_BPE */
    int pre;                        /* BPE: how text splits into words (PRE_...) */
    int n;
    Str *text;                      /* each token's text (in the file) */
    float *score;                   /* SPM: each token's score */
    unsigned char *attr;            /* each token's type (TOK_...) */
    Hash ids;                       /* text -> token */
    Str *merges;                    /* BPE: "a b", in order */
    int n_merges;
    Hash ranks;                     /* "a b" -> its place in merges */
    int bos, eos, unk;              /* -1 if none */
    int add_bos, add_eos, add_space;
    int byte_token[256];            /* SPM: <0x00>... (-1 if none) */
    int *special;                   /* the control, user-defined and unknown tokens, longest first */
    int n_special;
    int eog[16];                    /* tokens that end a generation */
    int n_eog;
    int ignore_merges;              /* llama 3: a word that is a token is that token */
    int rstrip;                     /* phi3: white space after a special token goes (but <s>'s...) */
} Vocab;

/* GPT-2's characters for bytes (and back), made by the plugin's init */
static uint16_t BYTE_CHAR[256];
static int16_t CHAR_BYTE[256 + 68];

static void bpe_tables(void) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        int self = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        BYTE_CHAR[b] = (uint16_t)(self ? b : 256 + n++);
    }
    for (int c = 0; c < 256 + 68; c++) CHAR_BYTE[c] = -1;
    for (int b = 0; b < 256; b++) CHAR_BYTE[BYTE_CHAR[b]] = (int16_t)b;
}

static void vocab_free(Vocab *v) {
    free(v->text);
    free(v->score);
    free(v->attr);
    free(v->ids.slot);
    free(v->merges);
    free(v->ranks.slot);
    free(v->special);
    memset(v, 0, sizeof *v);
}

static int vocab_find(const Vocab *v, const char *s, size_t n) { return hash_find(&v->ids, s, n); }

static const Vocab *sort_vocab;  /* (qsort's comparison needs it: set under special_lock) */
static LubeLock *special_lock;

static int longer_first(const void *a, const void *b) {
    size_t na = sort_vocab->text[*(const int *)a].n, nb = sort_vocab->text[*(const int *)b].n;
    return na != nb ? (na < nb ? 1 : -1) : *(const int *)a - *(const int *)b;
}

static int vocab_is_eog(const Vocab *v, int id) {
    for (int i = 0; i < v->n_eog; i++)
        if (v->eog[i] == id) return 1;
    return 0;
}

static int meta_id(const Gguf *g, const char *key, int n) {
    double d = -1;
    return kv_num(g, key, &d) && d >= 0 && d < n ? (int)d : -1;
}

static int meta_flag(const Gguf *g, const char *key, int fallback) {
    double d = 0;
    return kv_num(g, key, &d) ? d != 0 : fallback;
}

/* The tokenizer from the metadata: 1, or 0 with a message in err. */
static int vocab_load(Vocab *v, const Gguf *g, char *err, size_t errsize) {
    Str model = {"", 0};
    memset(v, 0, sizeof *v);
    if (!kv_str(g, "tokenizer.ggml.model", &model)) {
        snprintf(err, errsize, "has no tokenizer (tokenizer.ggml.model)");
        return 0;
    }
    if (str_is(model, "llama")) v->kind = VOCAB_SPM;
    else if (str_is(model, "gpt2")) v->kind = VOCAB_BPE;
    else {
        snprintf(err, errsize, "has a tokenizer of kind '%.*s' (llm knows llama's and gpt2's)", (int)model.n, model.s);
        return 0;
    }
    const Kv *tokens = kv_array(g, "tokenizer.ggml.tokens", GV_STR);
    if (!tokens || tokens->count == 0 || tokens->count > 0x7fffffff) {
        snprintf(err, errsize, "has no tokens (tokenizer.ggml.tokens)");
        return 0;
    }
    v->n = (int)tokens->count;
    v->text = kv_strings(tokens);
    v->score = (float *)calloc((size_t)v->n, sizeof(float));
    v->attr = (unsigned char *)malloc((size_t)v->n);
    if (!v->score || !v->attr) abort();  /* LCOV_EXCL_LINE: out of memory */
    const Kv *scores = kv_array(g, "tokenizer.ggml.scores", GV_F32);
    const Kv *types = kv_array(g, "tokenizer.ggml.token_type", GV_I32);
    for (int i = 0; i < v->n; i++) {
        v->score[i] = scores && (uint64_t)i < scores->count ? (float)num_at(GV_F32, scores->at + 4 * i) : 0;
        int t = types && (uint64_t)i < types->count ? (int)num_at(GV_I32, types->at + 4 * i) : TOK_NORMAL;
        v->attr[i] = (unsigned char)(t >= TOK_NORMAL && t <= TOK_BYTE ? t : TOK_NORMAL);
    }
    hash_build(&v->ids, v->text, v->n);
    v->bos = meta_id(g, "tokenizer.ggml.bos_token_id", v->n);
    v->eos = meta_id(g, "tokenizer.ggml.eos_token_id", v->n);
    v->unk = meta_id(g, "tokenizer.ggml.unknown_token_id", v->n);
    v->add_bos = meta_flag(g, "tokenizer.ggml.add_bos_token", v->kind == VOCAB_SPM);
    v->add_eos = meta_flag(g, "tokenizer.ggml.add_eos_token", 0);
    v->add_space = meta_flag(g, "tokenizer.ggml.add_space_prefix", v->kind == VOCAB_SPM);
    Str arch = {"", 0};
    kv_str(g, "general.architecture", &arch);
    v->rstrip = str_is(arch, "phi3");  /* (as Hugging Face's tokenizer has it, and llama.cpp) */
    if (v->kind == VOCAB_BPE) {
        const Kv *merges = kv_array(g, "tokenizer.ggml.merges", GV_STR);
        if (!merges || merges->count > 0x7fffffff) {
            snprintf(err, errsize, "has no merges for its tokenizer (tokenizer.ggml.merges)");
            return 0;
        }
        v->n_merges = (int)merges->count;
        v->merges = kv_strings(merges);
        hash_build(&v->ranks, v->merges, v->n_merges);
        Str pre = {"", 0};
        kv_str(g, "tokenizer.ggml.pre", &pre);
        v->pre = str_is(pre, "llama-bpe") || str_is(pre, "llama3") || str_is(pre, "llama-v3") ? PRE_LLAMA3
               : str_is(pre, "qwen2") || str_is(pre, "deepseek-r1-qwen") ? PRE_QWEN2
               : str_is(pre, "smollm") ? PRE_SMOLLM : PRE_GPT2;
        v->ignore_merges = v->pre == PRE_LLAMA3;
    }
    char name[8];
    for (int b = 0; b < 256; b++) {
        snprintf(name, sizeof name, "<0x%02X>", b);
        v->byte_token[b] = vocab_find(v, name, 6);
        if (v->byte_token[b] < 0) v->byte_token[b] = vocab_find(v, (const char *)&(unsigned char){(unsigned char)b}, 1);
    }
    v->special = (int *)malloc(sizeof(int) * (size_t)v->n);
    if (!v->special) abort();  /* LCOV_EXCL_LINE: out of memory */
    static const char *const ends[] = {"<|eot_id|>", "<|im_end|>", "<|end|>", "<|endoftext|>", "<end_of_turn>",
                                       "<|end_of_text|>", "<|eom_id|>", "<EOT>"};
    int eog_ids[3] = {v->eos, meta_id(g, "tokenizer.ggml.eot_token_id", v->n), meta_id(g, "tokenizer.ggml.eom_token_id", v->n)};
    for (int i = 0; i < 3; i++)
        if (eog_ids[i] >= 0) v->eog[v->n_eog++] = eog_ids[i];
    for (int i = 0; i < v->n; i++) {
        int a = v->attr[i];
        if ((a == TOK_CONTROL || a == TOK_USER || a == TOK_UNKNOWN) && v->text[i].n) v->special[v->n_special++] = i;
        for (size_t e = 0; a == TOK_CONTROL && e < sizeof ends / sizeof ends[0] && v->n_eog < 16; e++)
            if (str_is(v->text[i], ends[e]) && !vocab_is_eog(v, i)) v->eog[v->n_eog++] = i;
    }
    lube->lock_acquire(special_lock);
    sort_vocab = v;
    qsort(v->special, (size_t)v->n_special, sizeof(int), longer_first);
    lube->lock_release(special_lock);
    return 1;
}

/* ---------- merging pairs ---------- */

typedef struct {
    float score;     /* higher first (SPM: the token's score; BPE: minus the merge's rank) */
    int left, right;
    size_t size;     /* the two symbols' length when it was made: it is out of date if that changed */
} Bigram;

typedef struct {
    Bigram *a;
    int n, cap;
} Heap;

static int bigram_before(const Bigram *x, const Bigram *y) {
    return x->score > y->score || (x->score == y->score && x->left < y->left);
}

static void heap_push(Heap *h, Bigram b) {
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->a = (Bigram *)realloc(h->a, sizeof(Bigram) * (size_t)h->cap);
        if (!h->a) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    int i = h->n++;
    while (i > 0 && bigram_before(&b, &h->a[(i - 1) / 2])) {
        h->a[i] = h->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h->a[i] = b;
}

static Bigram heap_pop(Heap *h) {
    Bigram top = h->a[0], last = h->a[--h->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n) break;
        if (c + 1 < h->n && bigram_before(&h->a[c + 1], &h->a[c])) c++;
        if (!bigram_before(&h->a[c], &last)) break;
        h->a[i] = h->a[c];
        i = c;
    }
    if (h->n) h->a[i] = last;
    return top;
}

typedef struct {
    int prev, next;
    size_t at, n;    /* where in the word, and how long (0: merged into the one before) */
} Sym;

/* what tokenizing needs, kept from word to word */
typedef struct {
    const Vocab *v;
    const char *word;
    Sym *sym;
    int n_sym, cap_sym;
    Heap heap;
    Text tmp;
} Work;

static void work_free(Work *w) {
    free(w->sym);
    free(w->heap.a);
    free(w->tmp.s);
}

/* The word's characters as symbols. */
static void split_chars(Work *w, const char *s, size_t n) {
    w->word = s;
    w->n_sym = 0;
    for (size_t at = 0; at < n;) {
        size_t k = (size_t)utf8_len((unsigned char)s[at]);
        if (k > n - at) k = n - at;
        if (w->n_sym == w->cap_sym) {
            w->cap_sym = w->cap_sym ? w->cap_sym * 2 : 64;
            w->sym = (Sym *)realloc(w->sym, sizeof(Sym) * (size_t)w->cap_sym);
            if (!w->sym) abort();  /* LCOV_EXCL_LINE: out of memory */
        }
        Sym *y = &w->sym[w->n_sym];
        y->prev = w->n_sym - 1;
        y->next = at + k == n ? -1 : w->n_sym + 1;
        y->at = at;
        y->n = k;
        w->n_sym++;
        at += k;
    }
}

/* Considers merging symbols left and right. */
static void try_pair(Work *w, int left, int right) {
    if (left < 0 || right < 0) return;
    const Sym *l = &w->sym[left], *r = &w->sym[right];
    Bigram b = {0, left, right, l->n + r->n};
    if (w->v->kind == VOCAB_SPM) {
        int id = vocab_find(w->v, w->word + l->at, b.size);
        if (id < 0) return;
        b.score = w->v->score[id];
    } else {
        w->tmp.n = 0;
        text_add(&w->tmp, w->word + l->at, l->n);
        text_add(&w->tmp, " ", 1);
        text_add(&w->tmp, w->word + r->at, r->n);
        int rank = hash_find(&w->v->ranks, w->tmp.s, w->tmp.n);
        if (rank < 0) return;
        b.score = -(float)rank;
    }
    heap_push(&w->heap, b);
}

/* Merges the best pairs while any merge, then gives the tokens of what is left. */
static void merge_word(Work *w, Ids *out) {
    const Vocab *v = w->v;
    w->heap.n = 0;
    for (int i = 1; i < w->n_sym; i++) try_pair(w, i - 1, i);
    while (w->heap.n) {
        Bigram b = heap_pop(&w->heap);
        Sym *l = &w->sym[b.left], *r = &w->sym[b.right];
        if (l->n == 0 || r->n == 0 || l->n + r->n != b.size) continue;
        l->n += r->n;
        r->n = 0;
        l->next = r->next;
        if (r->next >= 0) w->sym[r->next].prev = b.left;
        try_pair(w, l->prev, b.left);
        try_pair(w, b.left, l->next);
    }
    for (int i = 0; i < w->n_sym; i++) {
        const Sym *y = &w->sym[i];
        if (!y->n) continue;
        int id = vocab_find(v, w->word + y->at, y->n);
        if (id >= 0) {
            ids_push(out, id);
            continue;
        }
        for (size_t j = 0; j < y->n; j++) {  /* no token for it: its bytes' tokens */
            unsigned char c = (unsigned char)w->word[y->at + j];
            int bt = v->kind == VOCAB_SPM ? v->byte_token[c] : vocab_find(v, (const char *)&c, 1);
            if (bt < 0 && v->kind == VOCAB_SPM) bt = v->unk;
            if (bt >= 0) ids_push(out, bt);
        }
    }
}

/* ---------- SentencePiece ---------- */

static void spm_text(Work *w, const char *s, size_t n, int space_first, Ids *out) {
    Text t = {NULL, 0, 0};
    if (space_first) text_add(&t, "\xe2\x96\x81", 3);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ') text_add(&t, "\xe2\x96\x81", 3);
        else text_add(&t, s + i, 1);
    }
    split_chars(w, t.s, t.n);
    merge_word(w, out);
    free(t.s);
}

/* ---------- byte-level BPE: words, by the model's rules ---------- */

typedef struct {
    const uint32_t *cp;
    const unsigned char *cls;
    int end;           /* where this run of characters ends */
} Chars;

static uint32_t cp_at(const Chars *c, int i) { return i < c->end ? c->cp[i] : 0xffffffffu; }
static int cls_at(const Chars *c, int i) { return i < c->end ? c->cls[i] : CH_END; }
static uint32_t ascii_lower(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* 's 't 're 've 'm 'll 'd at pos (any case if `nocase`): their length, else 0 */
static int contraction(const Chars *c, int pos, int nocase) {
    if (cp_at(c, pos) != '\'' || pos + 1 >= c->end) return 0;
    uint32_t a = cp_at(c, pos + 1), b = cp_at(c, pos + 2);
    if (nocase) {
        a = ascii_lower(a);
        b = ascii_lower(b);
    }
    if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
    if (pos + 2 < c->end && ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l'))) return 3;
    return 0;
}

/* Where the next word from pos ends: GPT-2's rules,
 *   's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+ */
static int word_gpt2(const Chars *c, int pos) {
    int k = contraction(c, pos, 0);
    if (k) return pos + k;
    int f = cp_at(c, pos) == ' ' ? cls_at(c, pos + 1) : cls_at(c, pos);
    if (f == CH_LETTER || f == CH_NUMBER || f == CH_OTHER) {
        pos += cp_at(c, pos) == ' ';
        while (cls_at(c, pos) == f) pos++;
        return pos;
    }
    int n = 0;
    while (cls_at(c, pos + n) == CH_SPACE) n++;
    if (n > 1 && pos + n < c->end) return pos + n - 1;
    return pos + (n ? n : 1);
}

/* llama 3's (digits 3 at a time) and qwen2's (one at a time):
 *   (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+ */
static int word_llama3(const Chars *c, int pos, int digits) {
    int k = contraction(c, pos, 1);
    if (k) return pos + k;
    uint32_t cp = cp_at(c, pos);
    int f = cls_at(c, pos);
    if (cp != '\r' && cp != '\n' && f != CH_NUMBER && (f == CH_LETTER || cls_at(c, pos + 1) == CH_LETTER)) {
        pos++;
        while (cls_at(c, pos) == CH_LETTER) pos++;
        return pos;
    }
    if (f == CH_NUMBER) {
        int start = pos;
        while (cls_at(c, pos) == CH_NUMBER && pos - start < digits) pos++;
        return pos;
    }
    int f2 = cp == ' ' ? cls_at(c, pos + 1) : f;
    if (f2 == CH_OTHER) {
        pos += cp == ' ';
        while (cls_at(c, pos) == CH_OTHER) pos++;
        while (cp_at(c, pos) == '\r' || cp_at(c, pos) == '\n') pos++;
        return pos;
    }
    int n = 0, last_nl = 0;
    while (cls_at(c, pos + n) == CH_SPACE) {
        if (cp_at(c, pos + n) == '\r' || cp_at(c, pos + n) == '\n') last_nl = pos + n + 1;
        n++;
    }
    if (last_nl) return last_nl;
    if (n > 1 && pos + n < c->end) return pos + n - 1;
    return pos + (n ? n : 1);
}

/* Byte-level BPE of a run of text: its words, each byte as a character, merged. */
static void bpe_text(Work *w, const char *s, size_t n, Ids *out) {
    uint32_t *cp = (uint32_t *)malloc(sizeof(uint32_t) * (n + 1));
    unsigned char *cls = (unsigned char *)malloc(n + 1);
    size_t *at = (size_t *)malloc(sizeof(size_t) * (n + 1));
    if (!cp || !cls || !at) abort();  /* LCOV_EXCL_LINE: out of memory */
    int nc = 0;
    for (size_t i = 0; i < n;) {
        int k;
        cp[nc] = utf8_get((const unsigned char *)s + i, n - i, &k);
        cls[nc] = (unsigned char)char_class(cp[nc]);
        at[nc++] = i;
        i += (size_t)k;
    }
    at[nc] = n;
    Text word = {NULL, 0, 0};
    for (int run = 0; run < nc;) {
        /* (smollm's digits are words of their own before the rules split the rest) */
        int stop = nc;
        if (w->v->pre == PRE_SMOLLM) {
            stop = run;
            if (cls[run] == CH_NUMBER) stop++;
            else while (stop < nc && cls[stop] != CH_NUMBER) stop++;
        }
        Chars c = {cp, cls, stop};
        for (int pos = run; pos < stop;) {
            int end = w->v->pre == PRE_LLAMA3 ? word_llama3(&c, pos, 3)
                    : w->v->pre == PRE_QWEN2 ? word_llama3(&c, pos, 1) : word_gpt2(&c, pos);
            word.n = 0;
            for (size_t i = at[pos]; i < at[end]; i++) {
                unsigned char u[2];
                text_add(&word, u, (size_t)utf8_put(BYTE_CHAR[(unsigned char)s[i]], u));
            }
            int whole = w->v->ignore_merges ? vocab_find(w->v, word.s, word.n) : -1;
            if (whole >= 0) {
                ids_push(out, whole);
            } else {
                split_chars(w, word.s, word.n);
                merge_word(w, out);
            }
            pos = end;
        }
        run = stop;
    }
    free(word.s);
    free(cp);
    free(cls);
    free(at);
}

/* ---------- tokenize and detokenize ---------- */

typedef struct {
    int token;      /* -1: text */
    size_t at, n;
} Frag;

/* The text in pieces: the special tokens in it, and the runs of text between them. *count is how many. */
static Frag *find_special(const Vocab *v, const char *s, size_t n, int all, int *count) {
    int cap = 16, nf = n ? 1 : 0;
    Frag *f = (Frag *)malloc(sizeof(Frag) * (size_t)cap);
    if (!f) abort();  /* LCOV_EXCL_LINE: out of memory */
    f[0].token = -1;
    f[0].at = 0;
    f[0].n = n;
    for (int k = 0; k < v->n_special; k++) {
        int id = v->special[k];
        if (!all && v->attr[id] != TOK_USER) continue;
        const Str *t = &v->text[id];
        for (int i = 0; i < nf; i++) {
            if (f[i].token >= 0 || f[i].n < t->n) continue;
            const char *p = s + f[i].at;
            size_t j = 0;
            while (j + t->n <= f[i].n && memcmp(p + j, t->s, t->n)) j++;
            if (j + t->n > f[i].n) continue;
            if (nf + 2 > cap) {
                cap *= 2;
                f = (Frag *)realloc(f, sizeof(Frag) * (size_t)cap);
                if (!f) abort();  /* LCOV_EXCL_LINE: out of memory */
            }
            /* [before][token][after]: the token and what follows go after fragment i (which is looked at next) */
            Frag after = {-1, f[i].at + j + t->n, f[i].n - j - t->n};
            if (v->rstrip && !str_is(*t, "<s>") && !str_is(*t, "<unk>") && !str_is(*t, "<|endoftext|>"))
                while (after.n && strchr(" \t\n\r\v\f", s[after.at]) && s[after.at]) {
                    after.at++;
                    after.n--;
                }
            Frag tok = {id, f[i].at + j, t->n};
            int add = 1 + (after.n > 0);
            memmove(&f[i + 1 + add], &f[i + 1], sizeof(Frag) * (size_t)(nf - i - 1));
            f[i].n = j;
            f[i + 1] = tok;
            if (after.n) f[i + 2] = after;
            nf += add;
            if (j == 0) {  /* (an empty run before it goes) */
                memmove(&f[i], &f[i + 1], sizeof(Frag) * (size_t)(nf - i - 1));
                nf--;
            }
        }
    }
    *count = nf;
    return f;
}

/* The tokens of text s (n bytes): with the BOS (and EOS) if asked, special tokens in it if `special`. */
static void tokenize(const Vocab *v, const char *s, size_t n, int bos, int eos, int special, Ids *out) {
    int nf;
    Frag *f = find_special(v, s, n, special, &nf);
    Work w = {v, NULL, NULL, 0, 0, {NULL, 0, 0}, {NULL, 0, 0}};
    if (bos && v->bos >= 0) ids_push(out, v->bos);
    int after_special = 1;
    for (int i = 0; i < nf; i++) {
        if (f[i].token >= 0) {
            ids_push(out, f[i].token);
            after_special = 1;
            continue;
        }
        if (v->kind == VOCAB_SPM) spm_text(&w, s + f[i].at, f[i].n, v->add_space && after_special, out);
        else bpe_text(&w, s + f[i].at, f[i].n, out);
        after_special = 0;
    }
    if (eos && v->eos >= 0) ids_push(out, v->eos);
    work_free(&w);
    free(f);
}

/* Adds token id's text to t: control tokens' only if `special`. */
static void token_piece(const Vocab *v, int id, int special, Text *t) {
    const Str *s = &v->text[id];
    int a = v->attr[id];
    if (a == TOK_CONTROL || a == TOK_UNKNOWN || a == TOK_UNUSED) {
        if (special) text_add(t, s->s, s->n);
    } else if (a == TOK_USER) {
        text_add(t, s->s, s->n);
    } else if (a == TOK_BYTE && v->kind == VOCAB_SPM && s->n == 6) {
        unsigned char b = (unsigned char)strtol(s->s + 3, NULL, 16);
        text_add(t, &b, 1);
    } else if (v->kind == VOCAB_SPM) {
        for (size_t i = 0; i < s->n; i++) {
            if (i + 3 <= s->n && !memcmp(s->s + i, "\xe2\x96\x81", 3)) {
                text_add(t, " ", 1);
                i += 2;
            } else {
                text_add(t, s->s + i, 1);
            }
        }
    } else {
        for (size_t i = 0; i < s->n;) {
            int k;
            uint32_t c = utf8_get((const unsigned char *)s->s + i, s->n - i, &k);
            if (c < 256 + 68 && CHAR_BYTE[c] >= 0) {
                unsigned char b = (unsigned char)CHAR_BYTE[c];
                text_add(t, &b, 1);
            } else {
                text_add(t, s->s + i, (size_t)k);
            }
            i += (size_t)k;
        }
    }
}

/* The text of tokens: SentencePiece's space in front of the first word is taken off again. */
static void detokenize(const Vocab *v, const int *ids, int n, int special, Text *t) {
    int strip = v->kind == VOCAB_SPM && v->add_space;
    for (int i = 0; i < n; i++) {
        size_t before = t->n;
        token_piece(v, ids[i], special, t);
        if (strip && t->n > before) {
            if (t->s[before] == ' ') {
                memmove(t->s + before, t->s + before + 1, t->n - before);
                t->n--;
            }
            strip = 0;
        }
    }
}
