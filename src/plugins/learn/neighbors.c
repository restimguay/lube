/*
 * learn/neighbors.c - models that keep what they learned from: k nearest
 * neighbors (a k-d tree for few features; for many, candidates from a
 * matrix product ranked by exact distances; with few samples, every
 * distance, eight queries at a time), and Gaussian naive Bayes (each
 * class's means and variances). Whichever way, the same neighbors: equally
 * near ones in the order of the training data. (Part of learn.c.)
 */

/* ======================================================================
 * k nearest neighbors
 * ==================================================================== */

static LubeValue fit_knn(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    memcpy(slot_make(m, S_X, 2, x->n, x->d, 0), x->v, sizeof(double) * (size_t)(x->n * x->d));
    if (m->kind == K_KNN_CLASSIFIER) {
        int64_t k;
        LubeValue err;
        int *codes = classes_fit(vm, "KNeighborsClassifier", m, y, 1, &k, &err);
        double *yv = slot_make(m, S_Y, 1, y->n, 1, 0);
        for (int64_t i = 0; i < y->n; i++) yv[i] = codes[i];
        free(codes);
    } else memcpy(slot_make(m, S_Y, y->nd, y->n, y->d, 0), y->v, sizeof(double) * (size_t)(y->n * y->d));
    return lube->nothing();
}

typedef struct {
    const double *train, *query;
    int64_t n_train, d, k, n;
    int manhattan;
    double *dist;      /* n x k, nearest first */
    int64_t *idx;
} KnnJob;

/* The squared Euclidean distance, in four sums: the same everywhere. */
static double distance(const double *a, const double *b, int64_t d) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int64_t f = 0;
    for (; f + 4 <= d; f += 4) {
        double t0 = a[f] - b[f], t1 = a[f + 1] - b[f + 1], t2 = a[f + 2] - b[f + 2], t3 = a[f + 3] - b[f + 3];
        s0 += t0 * t0;
        s1 += t1 * t1;
        s2 += t2 * t2;
        s3 += t3 * t3;
    }
    for (; f < d; f++) s0 += (a[f] - b[f]) * (a[f] - b[f]);
    return (s0 + s1) + (s2 + s3);
}

#define TILE 8  /* queries at once: each training sample is read once for all of them (and the loop vectorizes) */

/* Whether distance da of sample ia comes before db of ib: nearer, or as near and earlier. */
static int nearer(double da, int64_t ia, double db, int64_t ib) { return da < db || (da == db && ia < ib); }

/* Offers training sample t at distance dd to a query's k nearest so far (bd, bi: `have` of them, nearest first). */
static void knn_offer(double *bd, int64_t *bi, int64_t *have, int64_t k, double dd, int64_t t) {
    if (*have == k && !nearer(dd, t, bd[k - 1], bi[k - 1])) return;
    int64_t at = *have < k ? (*have)++ : k - 1;
    while (at > 0 && nearer(dd, t, bd[at - 1], bi[at - 1])) {
        bd[at] = bd[at - 1];
        bi[at] = bi[at - 1];
        at--;
    }
    bd[at] = dd;
    bi[at] = t;
}

static void knn_tiles(void *ctx, int64_t from, int64_t to) {
    KnnJob *j = (KnnJob *)ctx;
    int64_t d = j->d, have[TILE];
    double *qt = (double *)alloc(sizeof(double) * (size_t)(d * TILE)), acc[TILE];
    for (int64_t tile = from; tile < to; tile++) {
        int64_t q0 = tile * TILE, nq = j->n - q0 < TILE ? j->n - q0 : TILE;
        for (int64_t f = 0; f < d; f++)  /* (the tile's queries, a feature at a time) */
            for (int q = 0; q < TILE; q++) qt[f * TILE + q] = q < nq ? j->query[(q0 + q) * d + f] : 0;
        for (int q = 0; q < TILE; q++) have[q] = 0;
        for (int64_t t = 0; t < j->n_train; t++) {
            const double *tr = j->train + t * d;
            for (int q = 0; q < TILE; q++) acc[q] = 0;
            if (j->manhattan)
                for (int64_t f = 0; f < d; f++)
                    for (int q = 0; q < TILE; q++) acc[q] += fabs(qt[f * TILE + q] - tr[f]);
            else
                for (int64_t f = 0; f < d; f++)
                    for (int q = 0; q < TILE; q++) {
                        double diff = qt[f * TILE + q] - tr[f];
                        acc[q] += diff * diff;
                    }
            for (int q = 0; q < nq; q++) knn_offer(j->dist + (q0 + q) * j->k, j->idx + (q0 + q) * j->k, &have[q], j->k, acc[q], t);
        }
        for (int64_t i = q0 * j->k; !j->manhattan && i < (q0 + nq) * j->k; i++) j->dist[i] = sqrt(j->dist[i]);
    }
    free(qt);
}

/* ---------- a k-d tree, for few features: the same neighbors, found without every distance ---------- */

typedef struct {
    int64_t lo, hi;        /* its samples: perm[lo..hi) */
    int dim;               /* the feature it splits on, or -1 for a leaf */
    double split;          /* those before perm[mid] have at most this, those after at least */
    int64_t left, right;
} KdNode;

typedef struct {
    const KnnJob *j;
    KdNode *nodes;
    int64_t count, *perm;
} KdTree;

#define KD_LEAF 16

static double kd_at(const KdTree *t, int64_t p, int f) { return t->j->train[t->perm[p] * t->j->d + f]; }

/* Puts the sample of rank mid (by feature f) at perm[mid], the smaller ones before it and the larger after. */
static void kd_select(KdTree *t, int64_t lo, int64_t hi, int64_t mid, int f) {
    while (hi - lo > 1) {
        double pivot = kd_at(t, (lo + hi) / 2, f);
        int64_t a = lo, b = hi - 1;
        while (a <= b) {
            while (kd_at(t, a, f) < pivot) a++;
            while (kd_at(t, b, f) > pivot) b--;
            if (a <= b) {
                int64_t x = t->perm[a];
                t->perm[a++] = t->perm[b];
                t->perm[b--] = x;
            }
        }
        if (mid <= b) hi = b + 1;
        else if (mid >= a) lo = a;
        else return;
    }
}

static int64_t kd_build(KdTree *t, int64_t lo, int64_t hi) {
    int64_t id = t->count++, d = t->j->d;
    KdNode *nd = &t->nodes[id];
    nd->lo = lo;
    nd->hi = hi;
    nd->dim = -1;
    if (hi - lo <= KD_LEAF) return id;
    int best = 0;
    double spread = 0;
    for (int f = 0; f < d; f++) {  /* (the feature its samples spread most along) */
        double a = kd_at(t, lo, f), b = a;
        for (int64_t p = lo + 1; p < hi; p++) {
            a = fmin(a, kd_at(t, p, f));
            b = fmax(b, kd_at(t, p, f));
        }
        if (b - a > spread) {
            spread = b - a;
            best = f;
        }
    }
    if (spread == 0) return id;  /* (all the same: a leaf, however many) */
    int64_t mid = (lo + hi) / 2;
    kd_select(t, lo, hi, mid, best);
    nd->dim = best;
    nd->split = kd_at(t, mid, best);
    int64_t left = kd_build(t, lo, mid), right = kd_build(t, mid, hi);
    t->nodes[id].left = left;
    t->nodes[id].right = right;
    return id;
}

static void kd_query(const KdTree *t, int64_t id, const double *q, double *bd, int64_t *bi, int64_t *have) {
    const KdNode *nd = &t->nodes[id];
    const KnnJob *j = t->j;
    if (nd->dim < 0) {
        for (int64_t p = nd->lo; p < nd->hi; p++) {
            const double *tr = j->train + t->perm[p] * j->d;
            double s = 0;  /* (summed as the tiles sum: the same distances) */
            for (int64_t f = 0; f < j->d; f++) s += j->manhattan ? fabs(q[f] - tr[f]) : (q[f] - tr[f]) * (q[f] - tr[f]);
            knn_offer(bd, bi, have, j->k, s, t->perm[p]);
        }
        return;
    }
    double diff = q[nd->dim] - nd->split, bound = j->manhattan ? fabs(diff) : diff * diff;
    kd_query(t, diff <= 0 ? nd->left : nd->right, q, bd, bi, have);
    if (*have < j->k || bound <= bd[j->k - 1]) kd_query(t, diff <= 0 ? nd->right : nd->left, q, bd, bi, have);
}

static void kd_rows(void *ctx, int64_t from, int64_t to) {
    const KdTree *t = (const KdTree *)ctx;
    const KnnJob *j = t->j;
    for (int64_t i = from; i < to; i++) {
        int64_t have = 0;
        kd_query(t, 0, j->query + i * j->d, j->dist + i * j->k, j->idx + i * j->k, &have);
        for (int64_t c = 0; !j->manhattan && c < j->k; c++) j->dist[i * j->k + c] = sqrt(j->dist[i * j->k + c]);
    }
}

/* ---------- many features: candidates from a matrix product, ranked by their exact distances ---------- */

typedef struct {
    const KnnJob *j;
    const double *g;       /* rows q0.. of Q T^T (chunk x n_train) */
    const double *tn;      /* each training sample's squared norm */
    double tn_max;
    int64_t q0;
} GemmJob;

#define KNN_SPARE 8  /* candidates past k */

/* The exact squared distance of query q to training sample t, summed as the tiles sum it. */
static double exact_distance(const KnnJob *j, const double *q, int64_t t) {
    const double *tr = j->train + t * j->d;
    double s = 0;
    for (int64_t f = 0; f < j->d; f++) s += (q[f] - tr[f]) * (q[f] - tr[f]);
    return s;
}

static void gemm_rows(void *ctx, int64_t from, int64_t to) {
    const GemmJob *gj = (const GemmJob *)ctx;
    const KnnJob *j = gj->j;
    int64_t nt = j->n_train, k = j->k, kc = k + KNN_SPARE < nt ? k + KNN_SPARE : nt;
    double *cd = (double *)alloc(sizeof(double) * (size_t)kc), *ed = (double *)alloc(sizeof(double) * (size_t)k);
    int64_t *ci = (int64_t *)alloc(sizeof(int64_t) * (size_t)kc), *ei = (int64_t *)alloc(sizeof(int64_t) * (size_t)k);
    for (int64_t r = from; r < to; r++) {
        int64_t i = gj->q0 + r, have = 0, got = 0;
        const double *q = j->query + i * j->d, *g = gj->g + r * nt;
        for (int64_t t = 0; t < nt; t++) knn_offer(cd, ci, &have, kc, gj->tn[t] - 2 * g[t], t);
        for (int64_t c = 0; c < kc; c++) knn_offer(ed, ei, &got, k, exact_distance(j, q, ci[c]), ci[c]);
        /* exact unless a sample left out could come as near: its estimate is at least the last candidate's, and
         * estimates are off by at most err (rounding in the product and the norms) */
        double qn = 0;
        for (int64_t f = 0; f < j->d; f++) qn += q[f] * q[f];
        double err = 4.0 * (double)(j->d + 4) * DBL_EPSILON * (qn + gj->tn_max + 2 * sqrt(qn * gj->tn_max));
        if (kc < nt && !(qn + cd[kc - 1] - err > ed[k - 1])) {
            got = 0;  /* (too close to tell: every distance, exactly) */
            for (int64_t t = 0; t < nt; t++) knn_offer(ed, ei, &got, k, exact_distance(j, q, t), t);
        }
        for (int64_t c = 0; c < k; c++) {
            j->dist[i * k + c] = sqrt(ed[c]);
            j->idx[i * k + c] = ei[c];
        }
    }
    free(cd);
    free(ed);
    free(ci);
    free(ei);
}

/* The search for many features (Euclidean): chunks of queries times the training samples by array's matmul. */
static void knn_gemm(LubeVM *vm, KnnJob *j) {
    int64_t nt = j->n_train, d = j->d, chunk = (int64_t)(4e6 / (double)nt) + 1;
    double *tv, *tn = (double *)alloc(sizeof(double) * (size_t)nt), tn_max = 0;
    LubeValue train = new_f64(vm, nt, d, &tv);
    memcpy(tv, j->train, sizeof(double) * (size_t)(nt * d));
    for (int64_t t = 0; t < nt; t++) {
        double s = 0;
        for (int64_t f = 0; f < d; f++) s += tv[t * d + f] * tv[t * d + f];
        tn_max = fmax(tn_max, tn[t] = s);
    }
    LubeValue tt = arr->transpose(vm, train, NULL);
    for (int64_t q0 = 0; q0 < j->n; q0 += chunk) {
        int64_t rows = j->n - q0 < chunk ? j->n - q0 : chunk;
        double *qv;
        LubeValue qa = new_f64(vm, rows, d, &qv);
        memcpy(qv, j->query + q0 * d, sizeof(double) * (size_t)(rows * d));
        LubeValue g = arr->matmul(vm, qa, tt);
        GemmJob gj = {j, (const double *)arr->get(g)->data, tn, tn_max, q0};
        arr->parallel(rows, rows < 64 ? rows : 16, gemm_rows, &gj);
        lube->release(qa);
        lube->release(g);
    }
    lube->release(tt);
    lube->release(train);
    free(tn);
}

/* Each query's k nearest training samples: distances and indexes (new arrays, n x k). 0 with *err if k is more
 * than the samples fit had. */
static int knn_search(LubeVM *vm, const char *fname, const Model *m, const Data *x, int64_t k, double **dist,
                      int64_t **idx, LubeValue *err) {
    int64_t nt = m->s[S_X].shape[0];
    if (k > nt) {
        *err = lube->error(vm, "%s(): n_neighbors is %.0f, but fit had only %.0f samples", fname, (double)k, (double)nt);
        return 0;
    }
    KnnJob j = {m->s[S_X].v, x->v, nt, x->d, k, x->n, m->p[P_P] == 1, NULL, NULL};
    j.dist = *dist = (double *)alloc(sizeof(double) * (size_t)(x->n * k));
    j.idx = *idx = (int64_t *)alloc(sizeof(int64_t) * (size_t)(x->n * k));
    if (x->d <= 15 && nt >= 64) {  /* (few features: a k-d tree, as sklearn's "auto" takes then) */
        KdTree t = {&j, (KdNode *)alloc(sizeof(KdNode) * (size_t)(nt / 2 + 8)), 0, (int64_t *)alloc(sizeof(int64_t) * (size_t)nt)};
        for (int64_t i = 0; i < nt; i++) t.perm[i] = i;
        kd_build(&t, 0, nt);
        arr->parallel(x->n, x->n < 256 ? x->n : 64, kd_rows, &t);
        free(t.nodes);
        free(t.perm);
        return 1;
    }
    if (!j.manhattan && nt >= 64) {
        knn_gemm(vm, &j);
        return 1;
    }
    int64_t tiles = (x->n + TILE - 1) / TILE;
    arr->parallel(tiles, (double)nt * (double)x->d * (double)x->n < 1e6 ? tiles : 1, knn_tiles, &j);
    return 1;
}

/* The weights of a query's neighbors: 1 each, or 1 / distance (only the exact matches when there are some). */
static void knn_weights(const Model *m, const double *dist, int64_t k, double *w) {
    int exact = 0;
    for (int64_t c = 0; c < k; c++) exact |= dist[c] == 0;
    for (int64_t c = 0; c < k; c++)
        w[c] = m->p[P_WEIGHTS] == 0 ? 1 : exact ? dist[c] == 0 : 1 / dist[c];
}

/* The share of each class among each query's neighbors (weighted): n x classes. */
static double *knn_votes(LubeVM *vm, const Model *m, const Data *x, LubeValue *err) {
    int64_t k = (int64_t)m->p[P_N_NEIGHBORS], nc = m->s[S_CLASSES].shape[0];
    double *dist;
    int64_t *idx;
    if (!knn_search(vm, "predict", m, x, k, &dist, &idx, err)) return NULL;
    double *votes = (double *)alloc(sizeof(double) * (size_t)(x->n * nc)), *w = (double *)alloc(sizeof(double) * (size_t)k);
    for (int64_t i = 0; i < x->n; i++) {
        knn_weights(m, dist + i * k, k, w);
        double total = 0;
        for (int64_t c = 0; c < k; c++) {
            votes[i * nc + (int64_t)m->s[S_Y].v[idx[i * k + c]]] += w[c];
            total += w[c];
        }
        for (int64_t c = 0; c < nc; c++) votes[i * nc + c] /= total;
    }
    free(w);
    free(dist);
    free(idx);
    return votes;
}

static LubeValue predict_knn_class(LubeVM *vm, const Model *m, const Data *x) {
    LubeValue err;
    double *votes = knn_votes(vm, m, x, &err);
    if (!votes) return err;
    LubeValue r = argmax_labels(vm, m, votes, x->n, m->s[S_CLASSES].shape[0]);
    free(votes);
    return r;
}

static LubeValue proba_knn(LubeVM *vm, const Model *m, const Data *x) {
    LubeValue err;
    double *votes = knn_votes(vm, m, x, &err), *p;
    if (!votes) return err;
    int64_t nc = m->s[S_CLASSES].shape[0];
    LubeValue r = new_f64(vm, x->n, nc, &p);
    memcpy(p, votes, sizeof(double) * (size_t)(x->n * nc));
    free(votes);
    return r;
}

static LubeValue predict_knn_reg(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = (int64_t)m->p[P_N_NEIGHBORS], t = m->s[S_Y].nd == 2 ? m->s[S_Y].shape[1] : 1;
    double *dist, *out;
    int64_t *idx;
    LubeValue err;
    if (!knn_search(vm, "predict", m, x, k, &dist, &idx, &err)) return err;
    LubeValue r = new_f64(vm, x->n, t > 1 || m->s[S_Y].nd == 2 ? t : -1, &out);
    double *w = (double *)alloc(sizeof(double) * (size_t)k);
    for (int64_t i = 0; i < x->n; i++) {
        knn_weights(m, dist + i * k, k, w);
        double total = 0;
        for (int64_t c = 0; c < k; c++) total += w[c];
        for (int64_t o = 0; o < t; o++) {
            double s = 0;
            for (int64_t c = 0; c < k; c++) s += w[c] * m->s[S_Y].v[idx[i * k + c] * t + o];
            out[i * t + o] = s / total;
        }
    }
    free(w);
    free(dist);
    free(idx);
    return r;
}

/* kneighbors(X, n_neighbors?): [distances, indexes], each n x k, nearest first. */
static LubeValue kneighbors(LubeVM *vm, const Model *m, const Data *x, int64_t k) {
    double *dist, *dv;
    int64_t *idx, *iv;
    LubeValue err;
    if (!knn_search(vm, "kneighbors", m, x, k, &dist, &idx, &err)) return err;
    LubeValue list = lube->new_list(vm), d = new_f64(vm, x->n, k, &dv), i = new_i64(vm, x->n, k, &iv);
    memcpy(dv, dist, sizeof(double) * (size_t)(x->n * k));
    memcpy(iv, idx, sizeof(int64_t) * (size_t)(x->n * k));
    lube->list_push(list, d);
    lube->list_push(list, i);
    free(dist);
    free(idx);
    return list;
}

/* ======================================================================
 * Gaussian naive Bayes
 * ==================================================================== */

static LubeValue fit_gaussian_nb(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    int64_t k, d = x->d, n = x->n;
    LubeValue err;
    int *codes = classes_fit(vm, "GaussianNB", m, y, 1, &k, &err);
    double *theta = slot_make(m, S_THETA, 2, k, d, 0), *var = slot_make(m, S_VAR, 2, k, d, 0);
    double *count = slot_make(m, S_COUNT, 1, k, 1, 0), *prior = slot_make(m, S_PRIOR, 1, k, 1, 0);
    double *mean = (double *)alloc(sizeof(double) * (size_t)d), *all = (double *)alloc(sizeof(double) * (size_t)d);
    for (int64_t i = 0; i < n; i++) {
        count[codes[i]]++;
        for (int64_t j = 0; j < d; j++) {
            theta[codes[i] * d + j] += x->v[i * d + j];
            mean[j] += x->v[i * d + j];
        }
    }
    for (int64_t j = 0; j < d; j++) mean[j] /= (double)n;
    for (int64_t c = 0; c < k; c++)
        for (int64_t j = 0; j < d; j++) theta[c * d + j] /= count[c];
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < d; j++) {
            double t = x->v[i * d + j] - theta[codes[i] * d + j], u = x->v[i * d + j] - mean[j];
            var[codes[i] * d + j] += t * t;
            all[j] += u * u;
        }
    double top = 0;
    for (int64_t j = 0; j < d; j++) top = fmax(top, all[j] / (double)n);
    double eps = m->p[P_VAR_SMOOTHING] * top;
    for (int64_t c = 0; c < k; c++) {
        for (int64_t j = 0; j < d; j++) var[c * d + j] = var[c * d + j] / count[c] + eps;
        prior[c] = count[c] / (double)n;
    }
    slot_number(m, S_EPSILON, eps, 0);
    free(mean);
    free(all);
    free(codes);
    return lube->nothing();
}

/* Each sample's log probability in each class, up to a constant (n x k). */
static double *nb_joint(const Model *m, const Data *x) {
    int64_t k = m->s[S_CLASSES].shape[0], d = x->d;
    const double *theta = m->s[S_THETA].v, *var = m->s[S_VAR].v, *prior = m->s[S_PRIOR].v;
    double *jll = (double *)alloc(sizeof(double) * (size_t)(x->n * k)), *norm = (double *)alloc(sizeof(double) * (size_t)k);
    for (int64_t c = 0; c < k; c++) {
        double s = 0;
        for (int64_t j = 0; j < d; j++) s += log(6.283185307179586 * var[c * d + j]);
        norm[c] = log(prior[c]) - 0.5 * s;
    }
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t c = 0; c < k; c++) {
            double s = 0;
            for (int64_t j = 0; j < d; j++) {
                double t = x->v[i * d + j] - theta[c * d + j];
                s += t * t / var[c * d + j];
            }
            jll[i * k + c] = norm[c] - 0.5 * s;
        }
    free(norm);
    return jll;
}

static LubeValue predict_nb(LubeVM *vm, const Model *m, const Data *x) {
    double *jll = nb_joint(m, x);
    LubeValue r = argmax_labels(vm, m, jll, x->n, m->s[S_CLASSES].shape[0]);
    free(jll);
    return r;
}

static LubeValue proba_nb_as(LubeVM *vm, const Model *m, const Data *x, int logs) {
    int64_t k = m->s[S_CLASSES].shape[0];
    double *jll = nb_joint(m, x), *p;
    LubeValue r = new_f64(vm, x->n, k, &p);
    for (int64_t i = 0; i < x->n; i++) {
        double top = jll[i * k], sum = 0;
        for (int64_t c = 1; c < k; c++) top = fmax(top, jll[i * k + c]);
        for (int64_t c = 0; c < k; c++) sum += exp(jll[i * k + c] - top);
        for (int64_t c = 0; c < k; c++) {
            double lp = jll[i * k + c] - top - log(sum);
            p[i * k + c] = logs ? lp : exp(lp);
        }
    }
    free(jll);
    return r;
}

static LubeValue proba_nb(LubeVM *vm, const Model *m, const Data *x) { return proba_nb_as(vm, m, x, 0); }
static LubeValue log_proba_nb(LubeVM *vm, const Model *m, const Data *x) { return proba_nb_as(vm, m, x, 1); }
