/*
 * learn/transform.c - models that change X: PCA (the eigenvectors of the
 * covariance, signs as sklearn's svd_flip gives them), the scalers, the
 * one-hot encoder and polynomial features. (Part of learn.c.)
 */

/* ======================================================================
 * PCA
 * ==================================================================== */

static LubeValue fit_pca(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)y;
    int64_t n = x->n, d = x->d, r = n < d ? n : d;
    double want = m->p[P_N_COMPONENTS];
    if (want >= 1 && want > (double)r)
        return lube->error(vm, "fit(): PCA has n_components=%.0f, but X has only %.0f %s", want, (double)r,
                           n < d ? "samples" : "features");
    LubeValue keep;
    double *mean = slot_make(m, S_MEAN, 1, d, 1, 0);
    centered(vm, x, mean, &keep);
    double *vec = (double *)alloc(sizeof(double) * (size_t)(r * d)), *ev = (double *)alloc(sizeof(double) * (size_t)r);
    if (d <= n) {  /* the covariance's eigenvectors (d x d) */
        double *c = tmatmul(vm, keep, keep), *lam = (double *)alloc(sizeof(double) * (size_t)d);
        eigh(c, d, lam);
        for (int64_t i = 0; i < d; i++) {
            ev[i] = fmax(lam[d - 1 - i], 0);
            for (int64_t j = 0; j < d; j++) vec[i * d + j] = c[j * d + d - 1 - i];
        }
        free(c);
        free(lam);
    } else {  /* fewer samples than features: from the n x n Gram matrix, v = X^T u / sqrt(lambda) */
        LubeValue t = arr->transpose(vm, keep, NULL);
        double *g = tmatmul(vm, t, t), *lam = (double *)alloc(sizeof(double) * (size_t)n);
        const double *xc = (const double *)arr->get(keep)->data;
        lube->release(t);
        eigh(g, n, lam);
        for (int64_t i = 0; i < n; i++) {
            int64_t c = n - 1 - i;
            ev[i] = fmax(lam[c], 0);
            double norm = 0;
            for (int64_t j = 0; j < d; j++) {
                double s = 0;
                for (int64_t q = 0; q < n; q++) s += xc[q * d + j] * g[q * n + c];
                vec[i * d + j] = s;
                norm += s * s;
            }
            for (int64_t j = 0; j < d; j++) vec[i * d + j] = norm > 0 ? vec[i * d + j] / sqrt(norm) : 0;
        }
        free(g);
        free(lam);
    }
    lube->release(keep);
    double total = 0;
    for (int64_t i = 0; i < r; i++) {
        int64_t top = 0;  /* (the sign that makes each component's largest entry positive) */
        for (int64_t j = 1; j < d; j++)
            if (fabs(vec[i * d + j]) > fabs(vec[i * d + top])) top = j;
        if (vec[i * d + top] < 0)
            for (int64_t j = 0; j < d; j++) vec[i * d + j] = -vec[i * d + j];
        total += ev[i] /= (double)(n - 1 > 0 ? n - 1 : 1);
    }
    int64_t k = r;
    if (want >= 1) k = (int64_t)want;
    else if (want > 0) {  /* the fewest components explaining more than that share of the variance */
        double cum = 0;
        for (k = 0; k < r; k++)
            if ((cum += ev[k] / total) > want) break;
        k = k < r ? k + 1 : r;
    }
    double *comp = slot_make(m, S_COMPONENTS, 2, k, d, 0), *var = slot_make(m, S_EVAR, 1, k, 1, 0);
    double *ratio = slot_make(m, S_EVAR_RATIO, 1, k, 1, 0), *sing = slot_make(m, S_SINGULAR, 1, k, 1, 0);
    memcpy(comp, vec, sizeof(double) * (size_t)(k * d));
    double noise = 0;
    for (int64_t i = 0; i < r; i++) {
        if (i < k) {
            var[i] = ev[i];
            ratio[i] = ev[i] / total;
            sing[i] = sqrt(ev[i] * (double)(n - 1));
        } else noise += ev[i] / (double)(r - k);
    }
    slot_number(m, S_NOISE, noise, 0);
    slot_number(m, S_N_COMPONENTS, (double)k, 1);
    free(vec);
    free(ev);
    return lube->nothing();
}

static LubeValue transform_pca(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = m->s[S_COMPONENTS].shape[0], d = x->d;
    const double *c = m->s[S_COMPONENTS].v, *mean = m->s[S_MEAN].v, *ev = m->s[S_EVAR].v;
    double *out, *row = (double *)alloc(sizeof(double) * (size_t)d);
    LubeValue r = new_f64(vm, x->n, k, &out);
    for (int64_t i = 0; i < x->n; i++) {
        for (int64_t j = 0; j < d; j++) row[j] = x->v[i * d + j] - mean[j];
        for (int64_t q = 0; q < k; q++) {
            double s = 0;
            for (int64_t j = 0; j < d; j++) s += row[j] * c[q * d + j];
            out[i * k + q] = m->p[P_WHITEN] != 0 ? s / sqrt(ev[q]) : s;
        }
    }
    free(row);
    return r;
}

/* inverse_transform: back from k columns to the d features. */
static LubeValue inverse_pca(LubeVM *vm, const Model *m, const Data *z) {
    int64_t k = m->s[S_COMPONENTS].shape[0], d = m->n_features;
    if (z->d != k) return lube->error(vm, "inverse_transform(): X has %.0f columns, but PCA has %.0f components", (double)z->d, (double)k);
    const double *c = m->s[S_COMPONENTS].v, *mean = m->s[S_MEAN].v, *ev = m->s[S_EVAR].v;
    double *out;
    LubeValue r = new_f64(vm, z->n, d, &out);
    for (int64_t i = 0; i < z->n; i++)
        for (int64_t j = 0; j < d; j++) {
            double s = mean[j];
            for (int64_t q = 0; q < k; q++) s += z->v[i * k + q] * (m->p[P_WHITEN] != 0 ? sqrt(ev[q]) : 1) * c[q * d + j];
            out[i * d + j] = s;
        }
    return r;
}

/* ======================================================================
 * Scalers
 * ==================================================================== */

static LubeValue fit_standard(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)vm;
    (void)y;
    int64_t n = x->n, d = x->d;
    double *mean = slot_make(m, S_MEAN, 1, d, 1, 0), *var = slot_make(m, S_VAR, 1, d, 1, 0);
    double *scale = slot_make(m, S_SCALE, 1, d, 1, 0);
    for (int64_t i = 0; i < n * d; i++) mean[i % d] += x->v[i];
    for (int64_t j = 0; j < d; j++) mean[j] /= (double)n;
    for (int64_t i = 0; i < n * d; i++) var[i % d] += (x->v[i] - mean[i % d]) * (x->v[i] - mean[i % d]);
    for (int64_t j = 0; j < d; j++) {
        var[j] /= (double)n;
        double bound = (double)n * DBL_EPSILON * var[j] + ((double)n * mean[j] * DBL_EPSILON) * ((double)n * mean[j] * DBL_EPSILON);
        scale[j] = var[j] <= bound ? 1 : sqrt(var[j]);  /* (a constant feature is left as it is) */
    }
    slot_number(m, S_N_SEEN, (double)n, 1);
    return lube->nothing();
}

/* x scaled (or back, if inverse) by the StandardScaler or MinMaxScaler m. */
static LubeValue scale_as(LubeVM *vm, const Model *m, const Data *x, int inverse) {
    int64_t d = m->n_features;
    if (x->d != d)
        return lube->error(vm, "inverse_transform(): X has %.0f features, but %s was fitted with %.0f", (double)x->d,
                           KIND_NAMES[m->kind], (double)d);
    double *out;
    LubeValue r = new_f64(vm, x->n, d, &out);
    int standard = m->kind == K_STANDARD_SCALER;
    double lo = m->p[P_FEATURE_RANGE], hi = m->p[P_RANGE_HI];
    for (int64_t i = 0; i < x->n * d; i++) {
        int64_t j = i % d;
        double v = x->v[i];
        if (standard) {
            double mu = m->p[P_WITH_MEAN] != 0 ? m->s[S_MEAN].v[j] : 0, s = m->p[P_WITH_STD] != 0 ? m->s[S_SCALE].v[j] : 1;
            out[i] = inverse ? v * s + mu : (v - mu) / s;
        } else if (inverse) out[i] = (v - m->s[S_MIN].v[j]) / m->s[S_SCALE].v[j];
        else {
            out[i] = v * m->s[S_SCALE].v[j] + m->s[S_MIN].v[j];
            if (m->p[P_CLIP] != 0) out[i] = fmin(fmax(out[i], lo), hi);
        }
    }
    return r;
}

static LubeValue transform_scale(LubeVM *vm, const Model *m, const Data *x) { return scale_as(vm, m, x, 0); }
static LubeValue inverse_scale(LubeVM *vm, const Model *m, const Data *x) { return scale_as(vm, m, x, 1); }

static LubeValue fit_minmax(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)vm;
    (void)y;
    int64_t n = x->n, d = x->d;
    double *lo = slot_make(m, S_DATA_MIN, 1, d, 1, 0), *hi = slot_make(m, S_DATA_MAX, 1, d, 1, 0);
    double *range = slot_make(m, S_DATA_RANGE, 1, d, 1, 0), *scale = slot_make(m, S_SCALE, 1, d, 1, 0);
    double *min = slot_make(m, S_MIN, 1, d, 1, 0);
    for (int64_t j = 0; j < d; j++) lo[j] = hi[j] = x->v[j];
    for (int64_t i = 0; i < n * d; i++) {
        lo[i % d] = fmin(lo[i % d], x->v[i]);
        hi[i % d] = fmax(hi[i % d], x->v[i]);
    }
    for (int64_t j = 0; j < d; j++) {
        range[j] = hi[j] - lo[j];
        scale[j] = (m->p[P_RANGE_HI] - m->p[P_FEATURE_RANGE]) / (range[j] < 10 * DBL_EPSILON ? 1 : range[j]);
        min[j] = m->p[P_FEATURE_RANGE] - lo[j] * scale[j];
    }
    slot_number(m, S_N_SEEN, (double)n, 1);
    return lube->nothing();
}

/* ======================================================================
 * OneHotEncoder: each feature's distinct values become columns of 0 and 1
 * ==================================================================== */

/* Whether feature j drops its first category (drop "first", or "if_binary" with two). */
static int drops(const Model *m, int64_t j) {
    int64_t count = (int64_t)(m->s[S_CAT_START].v[j + 1] - m->s[S_CAT_START].v[j]);
    return m->p[P_DROP] == 1 || (m->p[P_DROP] == 2 && count == 2);
}

static int64_t onehot_columns(const Model *m) {
    int64_t cols = 0;
    for (int64_t j = 0; j < m->n_features; j++)
        cols += (int64_t)(m->s[S_CAT_START].v[j + 1] - m->s[S_CAT_START].v[j]) - drops(m, j);
    return cols;
}

static LubeValue fit_onehot(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)vm;
    (void)y;
    int64_t n = x->n, d = x->d, total = 0;
    double *col = (double *)alloc(sizeof(double) * (size_t)n), **cats = (double **)alloc(sizeof(double *) * (size_t)d);
    double *start = slot_make(m, S_CAT_START, 1, d + 1, 1, 0);
    for (int64_t j = 0; j < d; j++) {
        int64_t k;
        for (int64_t i = 0; i < n; i++) col[i] = x->v[i * d + j];
        cats[j] = distinct(col, n, &k);
        start[j + 1] = start[j] + (double)k;
        total += k;
    }
    double *all = slot_make(m, S_CATS, 1, total, 1, 0);
    for (int64_t j = 0; j < d; j++) {
        memcpy(all + (int64_t)start[j], cats[j], sizeof(double) * (size_t)(start[j + 1] - start[j]));
        free(cats[j]);
    }
    free(cats);
    free(col);
    m->n_features = d;
    slot_number(m, S_N_OUT, (double)onehot_columns(m), 1);
    return lube->nothing();
}

static LubeValue transform_onehot(LubeVM *vm, const Model *m, const Data *x) {
    int64_t d = x->d, cols = onehot_columns(m);
    const double *start = m->s[S_CAT_START].v, *cats = m->s[S_CATS].v;
    double *out;
    LubeValue r = new_f64(vm, x->n, cols, &out);
    for (int64_t i = 0; i < x->n; i++) {
        int64_t at = 0;
        for (int64_t j = 0; j < d; j++) {
            int64_t k = (int64_t)(start[j + 1] - start[j]), drop = drops(m, j);
            int64_t c = find_value(cats + (int64_t)start[j], k, x->v[i * d + j]);
            if (c < 0 && m->p[P_HANDLE_UNKNOWN] == 0) {
                char num[64];
                lube->format_number(x->v[i * d + j], num, sizeof num);
                lube->release(r);
                return lube->error(vm, "transform(): X has %s in column %.0f, which OneHotEncoder did not see in fit "
                                       "(handle_unknown: \"ignore\" gives zeros for it)", num, (double)j);
            }
            if (c >= drop) out[i * cols + at + c - drop] = 1;
            at += k - drop;
        }
    }
    return r;
}

static LubeValue inverse_onehot(LubeVM *vm, const Model *m, const Data *z) {
    int64_t d = m->n_features, cols = onehot_columns(m);
    if (z->d != cols)
        return lube->error(vm, "inverse_transform(): X has %.0f columns, but OneHotEncoder makes %.0f", (double)z->d, (double)cols);
    const double *start = m->s[S_CAT_START].v, *cats = m->s[S_CATS].v;
    double *out;
    LubeValue r = new_f64(vm, z->n, d, &out);
    for (int64_t i = 0; i < z->n; i++) {
        int64_t at = 0;
        for (int64_t j = 0; j < d; j++) {
            int64_t k = (int64_t)(start[j + 1] - start[j]), drop = drops(m, j), best = -1;
            for (int64_t c = 0; c < k - drop; c++)
                if (z->v[i * cols + at + c] > 0 && (best < 0 || z->v[i * cols + at + c] > z->v[i * cols + at + best])) best = c;
            if (best < 0 && !drop) {
                lube->release(r);
                return lube->error(vm, "inverse_transform(): row %.0f has no category for feature %.0f", (double)i, (double)j);
            }
            out[i * d + j] = cats[(int64_t)start[j] + (best < 0 ? 0 : best + drop)];
            at += k - drop;
        }
    }
    return r;
}

/* categories_: each feature's categories, sorted. */
static LubeValue onehot_categories(LubeVM *vm, const Model *m) {
    LubeValue list = lube->new_list(vm);
    const double *start = m->s[S_CAT_START].v;
    for (int64_t j = 0; j < m->n_features; j++) {
        int64_t k = (int64_t)(start[j + 1] - start[j]);
        lube->list_push(list, values_out(vm, m->s[S_CATS].v + (int64_t)start[j], 1, &k, m->label_dtype == ARR_I64));
    }
    return list;
}

/* ======================================================================
 * PolynomialFeatures: every product of features up to degree, in sklearn's
 * order (by degree, then the features' combinations in order)
 * ==================================================================== */

/* Adds the powers of each combination of `left` more features from `from` on (row: the powers so far). */
static void combinations(int64_t d, int64_t from, int left, int once, int64_t *row, double *powers, int64_t *count) {
    if (left == 0) {
        if (powers)
            for (int64_t j = 0; j < d; j++) powers[*count * d + j] = (double)row[j];
        (*count)++;
        return;
    }
    for (int64_t f = from; f < d; f++) {
        row[f]++;
        combinations(d, once ? f + 1 : f, left - 1, once, row, powers, count);
        row[f]--;
    }
}

static int64_t poly_powers(int64_t d, const Model *m, double *powers) {
    int64_t count = 0, *row = (int64_t *)alloc(sizeof(int64_t) * (size_t)d);
    for (int deg = m->p[P_INCLUDE_BIAS] != 0 ? 0 : 1; deg <= (int)m->p[P_DEGREE]; deg++)
        combinations(d, 0, deg, m->p[P_INTERACTION_ONLY] != 0, row, powers, &count);
    free(row);
    return count;
}

/* How many columns poly_powers makes (as a double: it may be huge). */
static double poly_count(int64_t d, const Model *m) {
    double total = 0;
    for (int deg = m->p[P_INCLUDE_BIAS] != 0 ? 0 : 1; deg <= (int)m->p[P_DEGREE]; deg++) {
        double c = 1;  /* d + deg - 1 choose deg (with repeats), or d choose deg */
        for (int i = 1; i <= deg; i++) c = c * (double)(m->p[P_INTERACTION_ONLY] != 0 ? d - i + 1 : d + i - 1) / i;
        total += c;
    }
    return total;
}

static LubeValue fit_poly(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    (void)y;
    int64_t d = x->d;
    double count = poly_count(d, m);
    if (count == 0) return lube->error(vm, "fit(): PolynomialFeatures with degree 0 and no bias makes no features");
    if (count > 1e6) return lube->error(vm, "fit(): PolynomialFeatures would make %.0f features", count);
    poly_powers(d, m, slot_make(m, S_POWERS, 2, (int64_t)count, d, 1));
    slot_number(m, S_N_OUT, (double)count, 1);
    return lube->nothing();
}

static LubeValue transform_poly(LubeVM *vm, const Model *m, const Data *x) {
    int64_t d = x->d, k = m->s[S_POWERS].shape[0];
    const double *pw = m->s[S_POWERS].v;
    double *out;
    LubeValue r = new_f64(vm, x->n, k, &out);
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t c = 0; c < k; c++) {
            double p = 1;
            for (int64_t j = 0; j < d; j++)
                for (int e = 0; e < (int)pw[c * d + j]; e++) p *= x->v[i * d + j];
            out[i * k + c] = p;
        }
    return r;
}
