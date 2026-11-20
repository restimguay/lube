/*
 * learn/tree.c - decision trees (CART, as sklearn's: the best split of
 * each node by Gini, entropy or squared error, X compared as float32) and
 * random forests of them (bootstrap samples, a random choice of features at
 * each node, trees built on several threads). (Part of learn.c.)
 */

#define FEATURE_THRESHOLD 1e-7  /* (closer values are the same, as sklearn has it) */
enum { CR_GINI, CR_ENTROPY, CR_SQUARED, CR_FRIEDMAN };

/* ---------- sorting a node's values, with its samples ---------- */

/* A float's bits as an unsigned number in the same order. */
static uint32_t float_key(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u & 0x80000000u ? ~u : u | 0x80000000u;
}

static float key_float(uint32_t k) {
    uint32_t u = k & 0x80000000u ? k & 0x7fffffffu : ~k;
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

typedef struct {
    uint32_t *key, *key2;  /* room for the most samples a node has */
    int64_t *s2;
} SortRoom;

/* Sorts n values x (and their samples s) ascending: by insertion when they are few, else by radix (a byte at a
 * time, the same order whatever the data, as no quicksort is). */
static void sort_values(float *x, int64_t *s, int64_t n, const SortRoom *room) {
    if (n <= 48) {
        for (int64_t i = 1; i < n; i++) {
            float v = x[i];
            int64_t sv = s[i], j = i;
            for (; j > 0 && x[j - 1] > v; j--) {
                x[j] = x[j - 1];
                s[j] = s[j - 1];
            }
            x[j] = v;
            s[j] = sv;
        }
        return;
    }
    uint32_t *ka = room->key, *kb = room->key2, *kt;
    int64_t *sa = s, *sb = room->s2, *st;
    for (int64_t i = 0; i < n; i++) ka[i] = float_key(x[i]);
    for (int shift = 0; shift < 32; shift += 8) {
        int64_t count[257] = {0};
        for (int64_t i = 0; i < n; i++) count[((ka[i] >> shift) & 255) + 1]++;
        if (count[((ka[0] >> shift) & 255) + 1] == n) continue;  /* (all the same byte) */
        for (int b = 0; b < 256; b++) count[b + 1] += count[b];
        for (int64_t i = 0; i < n; i++) {
            int64_t at = count[(ka[i] >> shift) & 255]++;
            kb[at] = ka[i];
            sb[at] = sa[i];
        }
        kt = ka;
        ka = kb;
        kb = kt;
        st = sa;
        sa = sb;
        sb = st;
    }
    if (sa != s) memcpy(s, sa, sizeof(int64_t) * (size_t)n);
    for (int64_t i = 0; i < n; i++) x[i] = key_float(ka[i]);
}

/* ---------- building one tree ---------- */

typedef struct {
    const float *xt;      /* X by feature: xt[f * n + i] */
    int64_t n, d;
    const int *codes;     /* each sample's class (a classifier), or NULL */
    const double *y;      /* each sample's target (a regressor) */
    int64_t k;            /* classes, or 1 */
    int criterion, max_depth, max_features;
    int64_t min_split, min_leaf;
} TreeData;

typedef struct {
    int64_t start, end;
    int depth, parent, is_left;
} Pending;

/* A node's statistics: the weight of each class (k), or the sums of y and y^2. */
typedef struct {
    double w, sum, sq;
    double *count;
} Stats;

static void stats_clear(Stats *s, int64_t k) {
    s->w = s->sum = s->sq = 0;
    for (int64_t c = 0; c < k; c++) s->count[c] = 0;
}

static void stats_add(const TreeData *td, Stats *s, int64_t i, double w) {
    s->w += w;
    if (td->codes) s->count[td->codes[i]] += w;
    else {
        s->sum += w * td->y[i];
        s->sq += w * td->y[i] * td->y[i];
    }
}

static double impurity_of(const TreeData *td, const Stats *s) {
    if (!td->codes) return s->sq / s->w - (s->sum / s->w) * (s->sum / s->w);
    double r = 0;
    if (td->criterion == CR_GINI) {
        for (int64_t c = 0; c < td->k; c++) r += s->count[c] * s->count[c];
        return 1.0 - r / (s->w * s->w);
    }
    for (int64_t c = 0; c < td->k; c++)
        if (s->count[c] > 0) {
            double p = s->count[c] / s->w;
            r -= p * (log(p) / log(2.0));
        }
    return r;
}

/* What the best split maximizes, for a left part l and a right part r (sklearn's proxy of the improvement). */
static double proxy_of(const TreeData *td, const Stats *l, const Stats *r) {
    if (td->criterion == CR_SQUARED) return l->sum * l->sum / l->w + r->sum * r->sum / r->w;
    if (td->criterion == CR_FRIEDMAN) {
        double diff = r->w * l->sum - l->w * r->sum;
        return diff * diff / (l->w * r->w);
    }
    return -r->w * impurity_of(td, r) - l->w * impurity_of(td, l);
}

static int tree_add(Tree *t, const Pending *p) {
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->nodes = (Node *)realloc(t->nodes, sizeof(Node) * (size_t)t->cap);
        t->value = (double *)realloc(t->value, sizeof(double) * (size_t)t->cap * (size_t)t->k);
        if (!t->nodes || !t->value) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    int id = t->count++;
    if (p->parent >= 0) {
        if (p->is_left) t->nodes[p->parent].left = id;
        else t->nodes[p->parent].right = id;
    }
    return id;
}

/* Builds t from the samples with weight (sw: NULL for 1 each), depth first (a node, its left side, its right). */
static void tree_build(const TreeData *td, const double *sw, Rng *rng, Tree *t) {
    int64_t n = td->n, d = td->d, k = td->k, m = 0;
    int64_t *samples = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    for (int64_t i = 0; i < n; i++)
        if (!sw || sw[i] > 0) samples[m++] = i;
    float *xf = (float *)alloc(sizeof(float) * (size_t)m);
    int *feats = (int *)alloc(sizeof(int) * (size_t)d);
    double *counts = (double *)alloc(sizeof(double) * (size_t)k * 3);
    SortRoom room = {(uint32_t *)alloc(sizeof(uint32_t) * (size_t)m), (uint32_t *)alloc(sizeof(uint32_t) * (size_t)m),
                     (int64_t *)alloc(sizeof(int64_t) * (size_t)m)};
    Stats all = {0, 0, 0, counts}, left = {0, 0, 0, counts + k}, right = {0, 0, 0, counts + 2 * k};
    Pending *stack = (Pending *)alloc(sizeof(Pending) * (size_t)(m + 1) * 2);
    int top = 0, depth_cap = td->max_depth < 0 ? 1 << 30 : td->max_depth;
    t->k = (int)k;
    t->importance = (double *)alloc(sizeof(double) * (size_t)d);
    stack[top++] = (Pending){0, m, 0, -1, 0};
    while (top > 0) {
        Pending p = stack[--top];
        int id = tree_add(t, &p);
        Node *node = &t->nodes[id];
        stats_clear(&all, k);
        for (int64_t q = p.start; q < p.end; q++) stats_add(td, &all, samples[q], sw ? sw[samples[q]] : 1);
        node->feature = -1;
        node->left = node->right = -1;
        node->threshold = -2;
        node->n = p.end - p.start;
        node->wn = all.w;
        node->impurity = impurity_of(td, &all);
        for (int64_t c = 0; c < k; c++) t->value[(size_t)id * (size_t)k + (size_t)c] = td->codes ? all.count[c] / all.w : all.sum / all.w;
        if (p.depth > t->depth) t->depth = p.depth;
        if (p.depth >= depth_cap || node->n < td->min_split || node->n < 2 * td->min_leaf || node->impurity <= DBL_EPSILON)
            continue;
        /* the best split: each feature's values sorted, every place between two of them */
        double best = -INFINITY, best_threshold = 0;
        int best_feature = -1;
        int64_t visited = 0, constant = 0, left_n = d;
        for (int64_t f = 0; f < d; f++) feats[f] = (int)f;
        while (left_n > 0 && (td->max_features >= d || visited < td->max_features || visited <= constant)) {
            int f;
            if (td->max_features >= d) f = feats[d - left_n];
            else {
                int64_t j = rng_below(rng, left_n);
                f = feats[j];
                feats[j] = feats[left_n - 1];
                feats[left_n - 1] = f;
            }
            left_n--;
            visited++;
            const float *col = td->xt + (size_t)f * (size_t)n;
            for (int64_t q = p.start; q < p.end; q++) xf[q] = col[samples[q]];
            sort_values(xf + p.start, samples + p.start, p.end - p.start, &room);
            if (xf[p.end - 1] <= xf[p.start] + FEATURE_THRESHOLD) {
                constant++;
                continue;
            }
            stats_clear(&left, k);
            for (int64_t q = p.start; q < p.end;) {
                int64_t prev;
                do {
                    stats_add(td, &left, samples[q], sw ? sw[samples[q]] : 1);
                    prev = q++;
                } while (q < p.end && xf[q] <= xf[prev] + FEATURE_THRESHOLD);
                if (q == p.end) break;
                if (q - p.start < td->min_leaf || p.end - q < td->min_leaf) continue;
                right.w = all.w - left.w;
                right.sum = all.sum - left.sum;
                for (int64_t c = 0; td->codes && c < k; c++) right.count[c] = all.count[c] - left.count[c];
                double proxy = proxy_of(td, &left, &right);
                if (proxy > best) {
                    best = proxy;
                    best_feature = f;
                    best_threshold = xf[prev] / 2.0 + xf[q] / 2.0;
                }
            }
        }
        if (best_feature < 0) continue;
        /* the samples that go left first, then the others */
        const float *col = td->xt + (size_t)best_feature * (size_t)n;
        int64_t q = p.start, end = p.end;
        while (q < end) {
            if (col[samples[q]] <= best_threshold) q++;
            else {
                int64_t s = samples[q];
                samples[q] = samples[--end];
                samples[end] = s;
            }
        }
        node->feature = best_feature;
        node->threshold = best_threshold;
        stack[top++] = (Pending){q, p.end, p.depth + 1, id, 0};
        stack[top++] = (Pending){p.start, q, p.depth + 1, id, 1};
    }
    /* each split's decrease of impurity, for feature_importances_ */
    for (int i = 0; i < t->count; i++) {
        const Node *nd = &t->nodes[i];
        if (nd->feature < 0) continue;
        const Node *l = &t->nodes[nd->left], *r = &t->nodes[nd->right];
        t->importance[nd->feature] += nd->wn * nd->impurity - l->wn * l->impurity - r->wn * r->impurity;
    }
    free(samples);
    free(xf);
    free(feats);
    free(counts);
    free(stack);
    free(room.key);
    free(room.key2);
    free(room.s2);
}

/* The leaf sample row x (d values, compared as float32) ends in. */
static int tree_leaf(const Tree *t, const double *x) {
    int i = 0;
    while (t->nodes[i].feature >= 0) i = (float)x[t->nodes[i].feature] <= t->nodes[i].threshold ? t->nodes[i].left : t->nodes[i].right;
    return i;
}

/* ---------- trees and forests as models ---------- */

typedef struct {
    TreeData td;
    Tree *trees;
    uint64_t *seeds;
    int bootstrap;
} ForestJob;

static void forest_trees(void *ctx, int64_t from, int64_t to) {
    ForestJob *j = (ForestJob *)ctx;
    int64_t n = j->td.n;
    double *sw = j->bootstrap ? (double *)alloc(sizeof(double) * (size_t)n) : NULL;
    for (int64_t t = from; t < to; t++) {
        Rng rng = {j->seeds[t]};
        if (sw) {
            memset(sw, 0, sizeof(double) * (size_t)n);
            for (int64_t i = 0; i < n; i++) sw[rng_below(&rng, n)] += 1;
        }
        tree_build(&j->td, sw, &rng, &j->trees[t]);
    }
    free(sw);
}

/* max_features as a count, for d features. */
static int features_of(double mf, int64_t d) {
    double c = mf == -1 ? (double)d : mf == -2 ? floor(sqrt((double)d)) : mf == -3 ? floor(log((double)d) / log(2.0))
             : mf < 1 ? floor(mf * (double)d) : mf;
    return c < 1 ? 1 : c > (double)d ? (int)d : (int)c;
}

static LubeValue fit_tree(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    int forest = m->kind == K_FOREST_CLASSIFIER || m->kind == K_FOREST_REGRESSOR;
    int classify = m->kind == K_TREE_CLASSIFIER || m->kind == K_FOREST_CLASSIFIER;
    int64_t n = x->n, d = x->d, k = 1;
    LubeValue err;
    int *codes = classify ? classes_fit(vm, KIND_NAMES[m->kind], m, y, 1, &k, &err) : NULL;
    float *xt = (float *)alloc(sizeof(float) * (size_t)(n * d));
    for (int64_t i = 0; i < n; i++)
        for (int64_t f = 0; f < d; f++) xt[f * n + i] = (float)x->v[i * d + f];
    ForestJob j = {{xt, n, d, codes, y->v, k, classify ? (m->p[P_CRITERION] == 0 ? CR_GINI : CR_ENTROPY) : CR_SQUARED + (int)m->p[P_CRITERION_R],
                    (int)m->p[P_MAX_DEPTH], features_of(m->p[P_MAX_FEATURES], d), (int64_t)m->p[P_MIN_SAMPLES_SPLIT],
                    (int64_t)m->p[P_MIN_SAMPLES_LEAF]},
                   NULL, NULL, forest && m->p[P_BOOTSTRAP] != 0};
    int nt = forest ? (int)m->p[P_N_ESTIMATORS] : 1;
    j.trees = (Tree *)alloc(sizeof(Tree) * (size_t)nt);
    j.seeds = (uint64_t *)alloc(sizeof(uint64_t) * (size_t)nt);
    Mt *rng = mt_for(m->p[P_RANDOM_STATE]);
    for (int t = 0; t < nt; t++) j.seeds[t] = (uint64_t)mt_next32(rng) << 32 | mt_next32(rng);
    free(rng);
    arr->parallel(nt, nt > 1 && (double)n * (double)d * nt > 2e4 ? 1 : nt, forest_trees, &j);
    m->trees = j.trees;
    m->n_trees = nt;
    /* feature_importances_: each tree's, as shares, then their mean as shares */
    double *imp = slot_make(m, S_IMPORTANCES, 1, d, 1, 0);
    for (int t = 0; t < nt; t++) {
        double total = 0;
        for (int64_t f = 0; f < d; f++) total += j.trees[t].importance[f];
        for (int64_t f = 0; f < d && total > 0; f++) imp[f] += j.trees[t].importance[f] / total;
    }
    double total = 0;
    for (int64_t f = 0; f < d; f++) total += imp[f];
    for (int64_t f = 0; f < d && total > 0; f++) imp[f] /= total;
    free(xt);
    free(j.seeds);
    free(codes);
    return lube->nothing();
}

typedef struct {
    const Model *m;
    const Data *x;
    double *out;  /* n x k: the trees' mean */
} TreePredictJob;

static void tree_predict_rows(void *ctx, int64_t from, int64_t to) {
    TreePredictJob *j = (TreePredictJob *)ctx;
    int64_t k = j->m->trees[0].k;
    for (int64_t i = from; i < to; i++) {
        double *o = j->out + i * k;
        for (int t = 0; t < j->m->n_trees; t++) {
            const Tree *tr = &j->m->trees[t];
            const double *v = tr->value + (size_t)tree_leaf(tr, j->x->v + i * j->x->d) * (size_t)k;
            for (int64_t c = 0; c < k; c++) o[c] += v[c];
        }
        for (int64_t c = 0; c < k; c++) o[c] /= j->m->n_trees;
    }
}

/* The mean of the trees' leaves for each sample: n x k (class shares, or values). */
static double *tree_means(const Model *m, const Data *x) {
    TreePredictJob j = {m, x, (double *)alloc(sizeof(double) * (size_t)(x->n * m->trees[0].k))};
    arr->parallel(x->n, (double)x->n * m->n_trees < 1e5 ? x->n : 256, tree_predict_rows, &j);
    return j.out;
}

static LubeValue predict_tree(LubeVM *vm, const Model *m, const Data *x) {
    double *p = tree_means(m, x), *out;
    LubeValue r;
    if (m->kind == K_TREE_CLASSIFIER || m->kind == K_FOREST_CLASSIFIER) r = argmax_labels(vm, m, p, x->n, m->trees[0].k);
    else {
        r = new_f64(vm, x->n, -1, &out);
        memcpy(out, p, sizeof(double) * (size_t)x->n);
    }
    free(p);
    return r;
}

static LubeValue proba_tree(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = m->trees[0].k;
    double *p = tree_means(m, x), *out;
    LubeValue r = new_f64(vm, x->n, k, &out);
    memcpy(out, p, sizeof(double) * (size_t)(x->n * k));
    free(p);
    return r;
}

/* apply(X): the leaf each sample ends in (a tree's). */
static LubeValue tree_apply(LubeVM *vm, const Model *m, const Data *x) {
    int64_t *out;
    LubeValue r = new_i64(vm, x->n, -1, &out);
    for (int64_t i = 0; i < x->n; i++) out[i] = tree_leaf(&m->trees[0], x->v + i * x->d);
    return r;
}

/* tree_: the tree's nodes, as sklearn's arrays (a leaf's feature and threshold are -2, its children -1). */
static LubeValue tree_object(LubeVM *vm, const Tree *t) {
    LubeValue o = lube->new_object(vm);
    int64_t n = t->count, *feature, *left, *right, *count;
    double *threshold, *impurity, *wn, *value;
    lube->object_set(vm, o, "node_count", lube->number_value((double)n));
    lube->object_set(vm, o, "max_depth", lube->number_value(t->depth));
    lube->object_set(vm, o, "children_left", new_i64(vm, n, -1, &left));
    lube->object_set(vm, o, "children_right", new_i64(vm, n, -1, &right));
    lube->object_set(vm, o, "feature", new_i64(vm, n, -1, &feature));
    lube->object_set(vm, o, "threshold", new_f64(vm, n, -1, &threshold));
    lube->object_set(vm, o, "impurity", new_f64(vm, n, -1, &impurity));
    lube->object_set(vm, o, "n_node_samples", new_i64(vm, n, -1, &count));
    lube->object_set(vm, o, "weighted_n_node_samples", new_f64(vm, n, -1, &wn));
    lube->object_set(vm, o, "value", new_f64(vm, n, t->k, &value));
    for (int64_t i = 0; i < n; i++) {
        const Node *nd = &t->nodes[i];
        left[i] = nd->left;
        right[i] = nd->right;
        feature[i] = nd->feature < 0 ? -2 : nd->feature;
        threshold[i] = nd->threshold;
        impurity[i] = nd->impurity;
        count[i] = nd->n;
        wn[i] = nd->wn;
    }
    memcpy(value, t->value, sizeof(double) * (size_t)(n * t->k));
    return o;
}
