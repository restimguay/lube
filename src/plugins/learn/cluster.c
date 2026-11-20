/*
 * learn/cluster.c - clusters: k-means (k-means++ starts, then Lloyd's
 * iterations, as sklearn's: each block of samples on a thread) and DBSCAN
 * (every pair's distance, then sklearn's walk from each core sample).
 * (Part of learn.c.)
 */

/* ======================================================================
 * k-means
 * ==================================================================== */

typedef struct {
    const double *x, *centers;
    int64_t n, d, k;
    int64_t *labels;
    double *sums;      /* per block: k x d sums, then k counts, then the inertia */
    int update;
} LloydJob;

static void lloyd_blocks(void *ctx, int64_t from, int64_t to) {
    LloydJob *j = (LloydJob *)ctx;
    int64_t d = j->d, k = j->k, per = k * d + k + 1;
    for (int64_t b = from; b < to; b++) {
        double *sums = j->sums + b * per, *counts = sums + k * d;
        memset(sums, 0, sizeof(double) * (size_t)per);
        int64_t end = (b + 1) * BLOCK < j->n ? (b + 1) * BLOCK : j->n;
        for (int64_t i = b * BLOCK; i < end; i++) {
            const double *xi = j->x + i * d;
            int64_t best = 0;
            double bd = INFINITY;
            for (int64_t c = 0; c < k; c++) {
                double dd = distance(xi, j->centers + c * d, d);
                if (dd < bd) {
                    bd = dd;
                    best = c;
                }
            }
            j->labels[i] = best;
            counts[k] += bd;
            if (j->update) {
                counts[best] += 1;
                for (int64_t f = 0; f < d; f++) sums[best * d + f] += xi[f];
            }
        }
    }
}

/* Assigns each sample its nearest center (labels), and gives the inertia; with sums (k x d, then k counts), also
 * the sums of each cluster's samples. */
static double lloyd_step(const double *x, int64_t n, int64_t d, const double *centers, int64_t k, int64_t *labels,
                         double *sums) {
    int64_t nb = blocks_of(n), per = k * d + k + 1;
    LloydJob j = {x, centers, n, d, k, labels, (double *)alloc(sizeof(double) * (size_t)(nb * per)), sums != NULL};
    over_blocks(n, (double)(k * d), lloyd_blocks, &j);
    double inertia = 0;
    if (sums) memset(sums, 0, sizeof(double) * (size_t)(k * d + k));
    for (int64_t b = 0; b < nb; b++) {
        const double *s = j.sums + b * per;
        inertia += s[k * d + k];
        for (int64_t i = 0; sums && i < k * d + k; i++) sums[i] += s[i];
    }
    free(j.sums);
    return inertia;
}

typedef struct {
    const double *x, *cands;
    const double *closest;
    int64_t n, d, nc;
    double *parts;   /* per block: each candidate's potential */
    double *dist;    /* n x nc: each sample's distance to each candidate (the nearer of it and closest) */
} PlusJob;

static void plus_blocks(void *ctx, int64_t from, int64_t to) {
    PlusJob *j = (PlusJob *)ctx;
    for (int64_t b = from; b < to; b++) {
        double *part = j->parts + b * j->nc;
        memset(part, 0, sizeof(double) * (size_t)j->nc);
        int64_t end = (b + 1) * BLOCK < j->n ? (b + 1) * BLOCK : j->n;
        for (int64_t i = b * BLOCK; i < end; i++)
            for (int64_t c = 0; c < j->nc; c++) {
                double dd = fmin(j->closest[i], distance(j->x + i * j->d, j->cands + c * j->d, j->d));
                j->dist[c * j->n + i] = dd;
                part[c] += dd;
            }
    }
}

/* k-means++: the first center a random sample, each next the best of a few drawn as likely as their squared
 * distance to the nearest center so far (sklearn's greedy k-means++). */
static void kmeans_plus_plus(Mt *rng, const double *x, int64_t n, int64_t d, int64_t k, double *centers) {
    int64_t trials = 2 + (int64_t)log((double)k), nb = blocks_of(n);
    double *closest = (double *)alloc(sizeof(double) * (size_t)n), *cum = (double *)alloc(sizeof(double) * (size_t)n);
    double *u = (double *)alloc(sizeof(double) * (size_t)trials), *cands = (double *)alloc(sizeof(double) * (size_t)(trials * d));
    int64_t first = 0, *ids = (int64_t *)alloc(sizeof(int64_t) * (size_t)trials);
    double s = 0, u0 = mt_double(rng);  /* (numpy's choice(n, p): the first whose share of the cumulated sum passes u) */
    for (int64_t i = 0; i < n; i++) cum[i] = s += 1.0 / (double)n;
    while (first < n - 1 && !(cum[first] / cum[n - 1] > u0)) first++;
    memcpy(centers, x + first * d, sizeof(double) * (size_t)d);
    double pot = 0;
    for (int64_t i = 0; i < n; i++) {
        closest[i] = distance(x + i * d, centers, d);
        pot += closest[i];
    }
    PlusJob j = {x, cands, closest, n, d, trials, (double *)alloc(sizeof(double) * (size_t)(nb * trials)),
                 (double *)alloc(sizeof(double) * (size_t)(n * trials))};
    for (int64_t c = 1; c < k; c++) {
        mt_fill(rng, u, trials, 0, 0, 1);
        s = 0;
        for (int64_t i = 0; i < n; i++) cum[i] = s += closest[i];
        for (int64_t t = 0; t < trials; t++) {  /* (the first sample whose cumulated sum reaches u pot) */
            double r = u[t] * pot;
            int64_t lo = 0, hi = n - 1;
            while (lo < hi) {
                int64_t mid = (lo + hi) / 2;
                if (cum[mid] < r) lo = mid + 1;
                else hi = mid;
            }
            ids[t] = lo;
            memcpy(cands + t * d, x + lo * d, sizeof(double) * (size_t)d);
        }
        over_blocks(n, (double)(trials * d), plus_blocks, &j);
        int64_t best = 0;
        double best_pot = INFINITY;
        for (int64_t t = 0; t < trials; t++) {
            double p = 0;
            for (int64_t b = 0; b < nb; b++) p += j.parts[b * trials + t];
            if (p < best_pot) {
                best_pot = p;
                best = t;
            }
        }
        pot = best_pot;
        memcpy(closest, j.dist + best * n, sizeof(double) * (size_t)n);
        memcpy(centers + c * d, x + ids[best] * d, sizeof(double) * (size_t)d);
    }
    free(closest);
    free(cum);
    free(u);
    free(cands);
    free(ids);
    free(j.parts);
    free(j.dist);
}

/* k different samples of n, as numpy's choice(n, k, replace=False, p) draws them with p the same for all (what
 * sklearn's "random" start asks): rounds of draws, each sample taken the first time it comes. */
static void choice_distinct(Mt *rng, int64_t n, int64_t k, int64_t *out) {
    double *p = (double *)alloc(sizeof(double) * (size_t)n), *cdf = (double *)alloc(sizeof(double) * (size_t)n);
    double *u = (double *)alloc(sizeof(double) * (size_t)k);
    int64_t found = 0;
    for (int64_t i = 0; i < n; i++) p[i] = 1.0 / (double)n;
    while (found < k) {
        int64_t want = k - found, got = found;
        mt_fill(rng, u, want, 0, 0, 1);
        for (int64_t i = 0; i < found; i++) p[out[i]] = 0;
        double s = 0;
        for (int64_t i = 0; i < n; i++) cdf[i] = s += p[i];
        for (int64_t i = 0; i < n; i++) cdf[i] /= s;
        for (int64_t t = 0; t < want; t++) {
            int64_t lo = 0, hi = n - 1, seen = 0;  /* (the first whose cdf passes u) */
            while (lo < hi) {
                int64_t mid = (lo + hi) / 2;
                if (cdf[mid] > u[t]) hi = mid;
                else lo = mid + 1;
            }
            for (int64_t i = found; i < got; i++) seen |= out[i] == lo;
            if (!seen) out[got++] = lo;
        }
        found = got;
    }
    free(p);
    free(cdf);
    free(u);
}

/* One run of Lloyd's iterations from centers: the inertia, labels, and the iterations it took (*iters). */
static double lloyd(const double *x, int64_t n, int64_t d, int64_t k, double *centers, int64_t *labels, int max_iter,
                    double tol, int *iters) {
    double *sums = (double *)alloc(sizeof(double) * (size_t)(k * d + k)), *counts = sums + k * d;
    int64_t *old = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    double *dist = (double *)alloc(sizeof(double) * (size_t)n);
    int it;
    for (it = 0; it < max_iter; it++) {
        lloyd_step(x, n, d, centers, k, labels, sums);
        int64_t empty = 0;
        for (int64_t c = 0; c < k; c++) empty += counts[c] == 0;
        if (empty) {  /* each empty cluster takes the sample farthest from its center (sklearn's relocation) */
            for (int64_t i = 0; i < n; i++) dist[i] = distance(x + i * d, centers + labels[i] * d, d);
            for (int64_t c = 0; c < k; c++) {
                if (counts[c] != 0) continue;
                int64_t far = 0;
                for (int64_t i = 1; i < n; i++)
                    if (dist[i] > dist[far]) far = i;
                dist[far] = -1;
                int64_t from = labels[far];
                for (int64_t f = 0; f < d; f++) {
                    sums[from * d + f] -= x[far * d + f];
                    sums[c * d + f] = x[far * d + f];
                }
                counts[from] -= 1;
                counts[c] = 1;
            }
        }
        double shift = 0;
        for (int64_t c = 0; c < k; c++)
            for (int64_t f = 0; f < d; f++) {
                double v = counts[c] > 0 ? sums[c * d + f] / counts[c] : centers[c * d + f], t = v - centers[c * d + f];
                shift += t * t;
                centers[c * d + f] = v;
            }
        if (it > 0 && !memcmp(old, labels, sizeof(int64_t) * (size_t)n)) {  /* (the labels did not change) */
            it++;
            break;
        }
        if (shift <= tol) {
            it++;
            break;
        }
        memcpy(old, labels, sizeof(int64_t) * (size_t)n);
    }
    double inertia = lloyd_step(x, n, d, centers, k, labels, NULL);  /* (labels for the last centers) */
    *iters = it;
    free(sums);
    free(old);
    free(dist);
    return inertia;
}

static LubeValue fit_kmeans(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)y;
    int64_t n = x->n, d = x->d, k = (int64_t)m->p[P_N_CLUSTERS];
    if (k > n) return lube->error(vm, "fit(): KMeans has n_clusters=%.0f, more than the %.0f samples", (double)k, (double)n);
    LubeValue keep;
    double *mean = (double *)alloc(sizeof(double) * (size_t)d), *xc = centered(vm, x, mean, &keep);
    double var = 0;  /* tol is relative to the mean variance of the features */
    for (int64_t i = 0; i < n * d; i++) var += xc[i] * xc[i];
    double tol = m->p[P_TOL] * var / (double)(n * d);
    double *centers = (double *)alloc(sizeof(double) * (size_t)(k * d)), *best_c = slot_make(m, S_CENTERS, 2, k, d, 0);
    int64_t *labels = (int64_t *)alloc(sizeof(int64_t) * (size_t)n), *perm = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    double *best_l = slot_make(m, S_LABELS, 1, n, 1, 1), best = INFINITY;
    int best_it = 0;
    Mt *rng = mt_for(m->p[P_RANDOM_STATE]);
    int runs = m->p[P_N_INIT] == -1 ? (m->p[P_INIT] == 0 ? 1 : 10) : (int)m->p[P_N_INIT];  /* ("auto": sklearn's) */
    for (int run = 0; run < runs; run++) {
        if (m->p[P_INIT] == 0) kmeans_plus_plus(rng, xc, n, d, k, centers);
        else {  /* "random": k different samples */
            choice_distinct(rng, n, k, perm);
            for (int64_t c = 0; c < k; c++) memcpy(centers + c * d, xc + perm[c] * d, sizeof(double) * (size_t)d);
        }
        int it;
        double inertia = lloyd(xc, n, d, k, centers, labels, (int)m->p[P_MAX_ITER], tol, &it);
        if (inertia < best) {
            best = inertia;
            best_it = it;
            memcpy(best_c, centers, sizeof(double) * (size_t)(k * d));
            for (int64_t i = 0; i < n; i++) best_l[i] = (double)labels[i];
        }
    }
    for (int64_t c = 0; c < k; c++)
        for (int64_t f = 0; f < d; f++) best_c[c * d + f] += mean[f];
    slot_number(m, S_INERTIA, best, 0);
    slot_number(m, S_N_ITER, best_it, 1);
    lube->release(keep);
    free(rng);
    free(mean);
    free(centers);
    free(labels);
    free(perm);
    return lube->nothing();
}

/* Each sample's nearest center: labels (and the inertia in *inertia). */
static int64_t *kmeans_labels(const Model *m, const Data *x, double *inertia) {
    int64_t *labels = (int64_t *)alloc(sizeof(int64_t) * (size_t)x->n);
    *inertia = lloyd_step(x->v, x->n, x->d, m->s[S_CENTERS].v, m->s[S_CENTERS].shape[0], labels, NULL);
    return labels;
}

static LubeValue predict_kmeans(LubeVM *vm, const Model *m, const Data *x) {
    double inertia;
    int64_t *labels = kmeans_labels(m, x, &inertia), *out;
    LubeValue r = new_i64(vm, x->n, -1, &out);
    memcpy(out, labels, sizeof(int64_t) * (size_t)x->n);
    free(labels);
    return r;
}

/* transform: each sample's distance to each center (n x k). */
static LubeValue transform_kmeans(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = m->s[S_CENTERS].shape[0];
    double *out;
    LubeValue r = new_f64(vm, x->n, k, &out);
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t c = 0; c < k; c++) out[i * k + c] = sqrt(distance(x->v + i * x->d, m->s[S_CENTERS].v + c * x->d, x->d));
    return r;
}

/* ======================================================================
 * DBSCAN
 * ==================================================================== */

typedef struct {
    const double *x;
    int64_t n, d;
    double eps2;
    int64_t *count;   /* each sample's neighbors (itself too) */
    int64_t *start;   /* where its neighbors go in `list` (NULL on the first pass, which only counts) */
    int64_t *list;
} RadiusJob;

static void radius_rows(void *ctx, int64_t from, int64_t to) {
    RadiusJob *j = (RadiusJob *)ctx;
    for (int64_t i = from; i < to; i++) {
        int64_t c = 0;
        for (int64_t t = 0; t < j->n; t++)
            if (distance(j->x + i * j->d, j->x + t * j->d, j->d) <= j->eps2) {
                if (j->start) j->list[j->start[i] + c] = t;
                c++;
            }
        j->count[i] = c;
    }
}

static LubeValue fit_dbscan(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)vm;
    (void)y;
    int64_t n = x->n, d = x->d, grain = (double)n * (double)n * (double)d < 1e6 ? n : 64;
    RadiusJob j = {x->v, n, d, m->p[P_EPS] * m->p[P_EPS], (int64_t *)alloc(sizeof(int64_t) * (size_t)n), NULL, NULL};
    arr->parallel(n, grain, radius_rows, &j);
    j.start = (int64_t *)alloc(sizeof(int64_t) * (size_t)(n + 1));
    for (int64_t i = 0; i < n; i++) j.start[i + 1] = j.start[i] + j.count[i];
    j.list = (int64_t *)alloc(sizeof(int64_t) * (size_t)j.start[n]);
    arr->parallel(n, grain, radius_rows, &j);
    double *labels = slot_make(m, S_LABELS, 1, n, 1, 1);
    int64_t *stack = (int64_t *)alloc(sizeof(int64_t) * (size_t)(j.start[n] + 1)), cores = 0, label = 0;
    for (int64_t i = 0; i < n; i++) {
        labels[i] = -1;
        cores += j.count[i] >= m->p[P_MIN_SAMPLES];
    }
    for (int64_t i0 = 0; i0 < n; i0++) {  /* (sklearn's dbscan_inner) */
        if (labels[i0] != -1 || j.count[i0] < m->p[P_MIN_SAMPLES]) continue;
        int64_t i = i0, top = 0;
        for (;;) {
            if (labels[i] == -1) {
                labels[i] = (double)label;
                if (j.count[i] >= m->p[P_MIN_SAMPLES])
                    for (int64_t q = j.start[i]; q < j.start[i + 1]; q++)
                        if (labels[j.list[q]] == -1) stack[top++] = j.list[q];
            }
            if (top == 0) break;
            i = stack[--top];
        }
        label++;
    }
    double *core = slot_make(m, S_CORE, 1, cores, 1, 1), *comp = slot_make(m, S_COMPONENTS, 2, cores, d, 0);
    for (int64_t i = 0, c = 0; i < n; i++)
        if (j.count[i] >= m->p[P_MIN_SAMPLES]) {
            memcpy(comp + c * d, x->v + i * d, sizeof(double) * (size_t)d);
            core[c++] = (double)i;
        }
    free(j.count);
    free(j.start);
    free(j.list);
    free(stack);
    return lube->nothing();
}
