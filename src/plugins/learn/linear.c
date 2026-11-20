/*
 * learn/linear.c - linear models: least squares, ridge, the lasso and the
 * elastic net (coordinate descent on the Gram matrix, stopping on the
 * duality gap, as sklearn does), logistic regression (L-BFGS on the mean
 * loss, binary or multinomial) and linear SVMs (the squared hinge, one
 * against the rest). (Part of learn.c.)
 */

/* coef_ and intercept_ from w (d x t: a column per target) and the means the data had (xm, ym: NULL for none). */
static void set_coef(Model *m, const double *w, int64_t d, int64_t t, int y2d, const double *xm, const double *ym) {
    double *coef = slot_make(m, S_COEF, y2d ? 2 : 1, y2d ? t : d, d, 0);
    double *icpt = y2d ? slot_make(m, S_INTERCEPT, 1, t, 1, 0) : slot_make(m, S_INTERCEPT, 0, 1, 1, 0);
    for (int64_t k = 0; k < t; k++) {
        double b = ym ? ym[k] : 0;
        for (int64_t j = 0; j < d; j++) {
            coef[k * d + j] = w[j * t + k];
            if (xm) b -= xm[j] * w[j * t + k];
        }
        icpt[k] = b;
    }
}

/* X and y centered (when fitting an intercept), with X^T X and X^T y: the parts every least squares fit needs. */
typedef struct {
    int64_t n, d, t;
    double *xm, *ym;  /* means (NULL without an intercept) */
    double *gram;     /* d x d */
    double *xty;      /* d x t */
    double *yy;       /* each target's sum of squares (centered) */
} Normal;

static void normal_free(Normal *e) {
    free(e->xm);
    free(e->ym);
    free(e->gram);
    free(e->xty);
    free(e->yy);
}

static void normal_of(LubeVM *vm, const Model *m, const Data *x, const Data *y, Normal *e) {
    int64_t n = x->n, d = x->d, t = y->nd == 2 ? y->d : 1;
    int icpt = m->p[P_FIT_INTERCEPT] != 0;
    e->n = n;
    e->d = d;
    e->t = t;
    e->xm = icpt ? (double *)alloc(sizeof(double) * (size_t)d) : NULL;
    e->ym = icpt ? (double *)alloc(sizeof(double) * (size_t)t) : NULL;
    LubeValue xk = icpt ? lube->nothing() : lube->retain(x->keep), yk;
    if (icpt) centered(vm, x, e->xm, &xk);
    double *yc;
    yk = new_f64(vm, n, y->nd == 2 ? t : -1, &yc);
    for (int64_t i = 0; i < n * t; i++) {
        yc[i] = y->v[i];
        if (icpt) e->ym[i % t] += y->v[i];
    }
    if (icpt) {
        for (int64_t k = 0; k < t; k++) e->ym[k] /= (double)n;
        for (int64_t i = 0; i < n * t; i++) yc[i] -= e->ym[i % t];
    }
    e->yy = (double *)alloc(sizeof(double) * (size_t)t);
    for (int64_t i = 0; i < n * t; i++) e->yy[i % t] += yc[i] * yc[i];
    e->gram = tmatmul(vm, xk, xk);
    e->xty = tmatmul(vm, xk, yk);
    lube->release(xk);
    lube->release(yk);
}

/* The least squares w (d x t) of the smallest norm, from the normal equations: through the eigenvectors of X^T X,
 * leaving out directions whose eigenvalue is lost in rounding (as lstsq does for small singular values). */
static double *least_squares(const Normal *e) {
    int64_t d = e->d, t = e->t;
    double *v = (double *)alloc(sizeof(double) * (size_t)(d * d)), *lam = (double *)alloc(sizeof(double) * (size_t)d);
    double *w = (double *)alloc(sizeof(double) * (size_t)(d * t)), *proj = (double *)alloc(sizeof(double) * (size_t)t);
    memcpy(v, e->gram, sizeof(double) * (size_t)(d * d));
    eigh(v, d, lam);
    double cut = fmax(lam[d - 1], 0) * (double)(d > e->n ? d : e->n) * DBL_EPSILON * 16;
    for (int64_t c = 0; c < d; c++) {
        if (!(lam[c] > cut)) continue;
        for (int64_t k = 0; k < t; k++) {
            proj[k] = 0;
            for (int64_t j = 0; j < d; j++) proj[k] += v[j * d + c] * e->xty[j * t + k];
        }
        for (int64_t j = 0; j < d; j++)
            for (int64_t k = 0; k < t; k++) w[j * t + k] += v[j * d + c] * proj[k] / lam[c];
    }
    free(v);
    free(lam);
    free(proj);
    return w;
}

static LubeValue fit_least_squares(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    Normal e;
    normal_of(vm, m, x, y, &e);
    double *w = NULL;
    if (m->kind == K_RIDGE && m->p[P_ALPHA] > 0) {  /* (X^T X + alpha I) w = X^T y */
        double *a = (double *)alloc(sizeof(double) * (size_t)(e.d * e.d));
        memcpy(a, e.gram, sizeof(double) * (size_t)(e.d * e.d));
        for (int64_t j = 0; j < e.d; j++) a[j * e.d + j] += m->p[P_ALPHA];
        if (cholesky(a, e.d)) {
            w = (double *)alloc(sizeof(double) * (size_t)(e.d * e.t));
            memcpy(w, e.xty, sizeof(double) * (size_t)(e.d * e.t));
            cholesky_solve(a, e.d, w, e.t);
        }
        free(a);
    }
    if (!w) {  /* (Ridge whose matrix rounding left singular: as least squares, with alpha added) */
        if (m->kind == K_RIDGE)
            for (int64_t j = 0; j < e.d; j++) e.gram[j * e.d + j] += m->p[P_ALPHA];
        w = least_squares(&e);
    }
    set_coef(m, w, e.d, e.t, y->nd == 2, e.xm, e.ym);
    free(w);
    normal_free(&e);
    return lube->nothing();
}

/* The duality gap of the elastic net at w (sklearn's gap_enet_gram), with XtA = X^T (y - X w) - beta w. */
static double enet_gap(int64_t d, const double *w, double alpha, double beta, const double *qw, const double *q,
                       double yy, double *xta, double *dual_norm) {
    double l2 = 0, qdw = 0, wqw = 0, l1 = 0;
    for (int64_t j = 0; j < d; j++) {
        if (beta > 0) l2 += w[j] * w[j];
        qdw += w[j] * q[j];
        wqw += w[j] * qw[j];
    }
    double r2 = yy + wqw - 2.0 * qdw, ry = yy - qdw;
    if (alpha == 0) {
        *dual_norm = 0;
        for (int64_t j = 0; j < d; j++) {
            xta[j] = q[j] - qw[j];
            *dual_norm += xta[j] * xta[j];
        }
        if (beta == 0) return *dual_norm;
        return r2 + 0.5 * beta * l2 - ry + 1 / (2 * beta) * *dual_norm;
    }
    *dual_norm = 0;
    for (int64_t j = 0; j < d; j++) {
        xta[j] = q[j] - qw[j] - beta * w[j];
        *dual_norm = fmax(*dual_norm, fabs(xta[j]));
        l1 += fabs(w[j]);
    }
    double primal = 0.5 * (r2 + beta * l2) + alpha * l1, scale = *dual_norm > alpha ? alpha / *dual_norm : 1.0;
    double dual = -0.5 * (scale * scale) * (r2 + beta * l2) + scale * ry;
    return primal - dual;
}

/* Lasso and ElasticNet: min 1/(2n) |y - Xw|^2 + alpha l1_ratio |w|_1 + alpha (1 - l1_ratio) / 2 |w|^2, by cyclic
 * coordinate descent on X^T X, for each target. */
static LubeValue fit_enet(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    Normal e;
    normal_of(vm, m, x, y, &e);
    int64_t d = e.d, t = e.t;
    double l1r = m->kind == K_LASSO ? 1.0 : m->p[P_L1_RATIO];
    double alpha = m->p[P_ALPHA] * l1r * (double)e.n, beta = m->p[P_ALPHA] * (1 - l1r) * (double)e.n;
    double *w = (double *)alloc(sizeof(double) * (size_t)(d * t)), *wk = (double *)alloc(sizeof(double) * (size_t)d);
    double *q = (double *)alloc(sizeof(double) * (size_t)d), *qw = (double *)alloc(sizeof(double) * (size_t)d);
    double *xta = (double *)alloc(sizeof(double) * (size_t)d);
    const double *g = e.gram;
    int iters = 0, max_iter = (int)m->p[P_MAX_ITER];
    for (int64_t k = 0; k < t; k++) {
        for (int64_t j = 0; j < d; j++) {
            q[j] = e.xty[j * t + k];
            wk[j] = 0;
            qw[j] = 0;
        }
        double tol = m->p[P_TOL] * e.yy[k], dual_norm, gap = enet_gap(d, wk, alpha, beta, qw, q, e.yy[k], xta, &dual_norm);
        int it = 0;
        if (!(gap >= 0 && gap <= tol)) {
            for (it = 0; it < max_iter; it++) {
                double w_max = 0, dw_max = 0;
                for (int64_t j = 0; j < d; j++) {
                    if (g[j * d + j] == 0.0) continue;
                    double old = wk[j], tmp = q[j] - qw[j] + old * g[j * d + j];
                    wk[j] = (tmp > 0 ? 1 : tmp < 0 ? -1 : 0) * fmax(fabs(tmp) - alpha, 0) / (g[j * d + j] + beta);
                    if (wk[j] != old)
                        for (int64_t i = 0; i < d; i++) qw[i] += (wk[j] - old) * g[j * d + i];
                    dw_max = fmax(dw_max, fabs(wk[j] - old));
                    w_max = fmax(w_max, fabs(wk[j]));
                }
                if (w_max == 0.0 || dw_max / w_max <= m->p[P_TOL] || it == max_iter - 1) {
                    gap = enet_gap(d, wk, alpha, beta, qw, q, e.yy[k], xta, &dual_norm);
                    if (gap <= tol) break;
                }
            }
            it = it < max_iter ? it + 1 : max_iter;
        }
        if (it > iters) iters = it;
        for (int64_t j = 0; j < d; j++) w[j * t + k] = wk[j];
    }
    set_coef(m, w, d, t, y->nd == 2, e.xm, e.ym);
    slot_number(m, S_N_ITER, iters, 1);
    free(w);
    free(wk);
    free(q);
    free(qw);
    free(xta);
    normal_free(&e);
    return lube->nothing();
}

/* a . b in four sums (the same everywhere, and faster than one). */
static double dot(const double *a, const double *b, int64_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int64_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

/* X w^T + b for each of k weight vectors (coef k x d, intercept k): n x k. */
static double *linear_scores(const Model *m, const Data *x) {
    const Slot *c = &m->s[S_COEF];
    int64_t k = c->nd == 2 ? c->shape[0] : 1, d = x->d;
    const double *icpt = m->s[S_INTERCEPT].v;
    double *z = (double *)alloc(sizeof(double) * (size_t)(x->n * k));
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t j = 0; j < k; j++) z[i * k + j] = icpt[j] + dot(x->v + i * d, c->v + j * d, d);
    return z;
}

/* A regressor's predictions: n, or n x t for several targets. */
static LubeValue predict_linear(LubeVM *vm, const Model *m, const Data *x) {
    const Slot *c = &m->s[S_COEF];
    int64_t k = c->nd == 2 ? c->shape[0] : 1;
    double *z = linear_scores(m, x), *out;
    LubeValue r = new_f64(vm, x->n, c->nd == 2 ? k : -1, &out);
    memcpy(out, z, sizeof(double) * (size_t)(x->n * k));
    free(z);
    return r;
}

/* ======================================================================
 * Logistic regression and linear SVMs: L-BFGS on all samples, the scores
 * and the gradient by array's matmul (the system's BLAS), the loss summed
 * in blocks on several threads
 * ==================================================================== */

typedef struct {
    LubeVM *vm;
    const Data *x;
    const int *codes;      /* each sample's class */
    int64_t k;             /* weight vectors: 1 (binary) or the classes (multinomial) */
    int svm;               /* squared hinge, against class `positive` (else the logistic loss) */
    int positive;
    int icpt;
    double penalty;        /* the L2 penalty's weight: 1 / (C n), or 1 / n for an SVM */
    double cost;           /* the loss's weight: 1 / n, or C / n for an SVM */
    int penalize_icpt;
    const double *w;       /* the point being looked at: k rows of d + 1 */
    double *z;             /* n x k: the scores X w^T, then the loss's slope in each */
    double *parts;         /* per block: the loss, then each intercept's slope */
} LossJob;

static double log1pexp(double z) { return z > 0 ? z + log1p(exp(-z)) : log1p(exp(z)); }

static void loss_blocks(void *ctx, int64_t from, int64_t to) {
    LossJob *j = (LossJob *)ctx;
    int64_t d = j->x->d, k = j->k;
    for (int64_t b = from; b < to; b++) {
        double *part = j->parts + b * (k + 1), loss = 0;
        memset(part, 0, sizeof(double) * (size_t)(k + 1));
        int64_t end = (b + 1) * BLOCK < j->x->n ? (b + 1) * BLOCK : j->x->n;
        for (int64_t i = b * BLOCK; i < end; i++) {
            double *z = j->z + i * k;
            for (int64_t c = 0; c < k && j->icpt; c++) z[c] += j->w[c * (d + 1) + d];
            if (j->svm) {  /* max(0, 1 - y z)^2 */
                double yi = j->codes[i] == j->positive ? 1 : -1, margin = 1 - yi * z[0];
                if (margin > 0) {
                    loss += margin * margin;
                    z[0] = -2 * margin * yi;
                } else z[0] = 0;
            } else if (k == 1) {  /* log(1 + e^z) - y z */
                double yi = j->codes[i] == 1;
                loss += log1pexp(z[0]) - yi * z[0];
                z[0] = (z[0] >= 0 ? 1 / (1 + exp(-z[0])) : exp(z[0]) / (1 + exp(z[0]))) - yi;
            } else {  /* log sum e^z - z_y */
                double top = z[0], sum = 0;
                for (int64_t c = 1; c < k; c++) top = fmax(top, z[c]);
                for (int64_t c = 0; c < k; c++) sum += exp(z[c] - top);
                double lse = top + log(sum);
                loss += lse - z[j->codes[i]];
                for (int64_t c = 0; c < k; c++) z[c] = exp(z[c] - lse) - (c == j->codes[i]);
            }
            for (int64_t c = 0; c < k; c++) part[1 + c] += z[c];
        }
        part[0] = loss;
    }
}

static double linear_loss(void *ctx, const double *w, double *grad) {
    LossJob *j = (LossJob *)ctx;
    int64_t n = j->x->n, d = j->x->d, k = j->k, np = k * (d + 1), nb = blocks_of(n);
    double *wt;
    LubeValue wv = new_f64(j->vm, d, k, &wt);
    for (int64_t c = 0; c < k; c++)
        for (int64_t f = 0; f < d; f++) wt[f * k + c] = w[c * (d + 1) + f];
    LubeValue zv = arr->matmul(j->vm, j->x->keep, wv);  /* (n x k: an Array of our own, changed in place) */
    j->z = (double *)arr->get(zv)->data;
    j->w = w;
    over_blocks(n, (double)k * 20, loss_blocks, j);
    LubeValue zt = arr->transpose(j->vm, zv, NULL), gv = arr->matmul(j->vm, zt, j->x->keep);
    const double *g = (const double *)arr->get(gv)->data;  /* (k x d: the slopes' sums over the samples) */
    double loss = 0;
    memset(grad, 0, sizeof(double) * (size_t)np);
    for (int64_t b = 0; b < nb; b++) {
        loss += j->parts[b * (k + 1)];
        for (int64_t c = 0; c < k; c++) grad[c * (d + 1) + d] += j->parts[b * (k + 1) + 1 + c];
    }
    for (int64_t c = 0; c < k; c++) memcpy(grad + c * (d + 1), g + c * d, sizeof(double) * (size_t)d);
    lube->release(wv);
    lube->release(zv);
    lube->release(zt);
    lube->release(gv);
    loss *= j->cost;
    for (int64_t i = 0; i < np; i++) {
        grad[i] *= j->cost;
        int is_icpt = i % (d + 1) == d;
        if (is_icpt && !j->icpt) grad[i] = 0;
        else if (!is_icpt || j->penalize_icpt) {
            loss += 0.5 * j->penalty * w[i] * w[i];
            grad[i] += j->penalty * w[i];
        }
    }
    return loss;
}

/* coef_ (k x d) and intercept_ (k) from the solver's rows of d + 1. */
static void set_linear_classes(Model *m, const double *w, int64_t k, int64_t d) {
    double *coef = slot_make(m, S_COEF, 2, k, d, 0), *icpt = slot_make(m, S_INTERCEPT, 1, k, 1, 0);
    for (int64_t c = 0; c < k; c++) {
        memcpy(coef + c * d, w + c * (d + 1), sizeof(double) * (size_t)d);
        icpt[c] = w[c * (d + 1) + d];
    }
}

static LubeValue fit_logistic(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    int64_t k, d = x->d;
    LubeValue err;
    int *codes = classes_fit(vm, "LogisticRegression", m, y, 0, &k, &err);
    if (!codes) return err;
    int64_t nw = k == 2 ? 1 : k, np = nw * (d + 1);
    LossJob j = {vm, x, codes, nw, 0, 0, m->p[P_FIT_INTERCEPT] != 0, 1 / (m->p[P_C] * (double)x->n), 1 / (double)x->n, 0,
                 NULL, NULL, NULL};
    j.parts = (double *)alloc(sizeof(double) * (size_t)(blocks_of(x->n) * (nw + 1)));
    double *w = (double *)alloc(sizeof(double) * (size_t)np);
    int it = lbfgs(linear_loss, &j, w, np, (int)m->p[P_MAX_ITER], m->p[P_TOL]);
    set_linear_classes(m, w, nw, d);
    slot_make(m, S_N_ITER, 1, 1, 1, 1)[0] = it;
    free(j.parts);
    free(w);
    free(codes);
    return lube->nothing();
}

/* LinearSVC: min 1/2 |w|^2 (the intercept too, as liblinear has it) + C sum max(0, 1 - y (w x + b))^2, one
 * problem for each class against the rest (one for two classes). */
static LubeValue fit_linear_svc(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    int64_t k, d = x->d;
    LubeValue err;
    int *codes = classes_fit(vm, "LinearSVC", m, y, 0, &k, &err);
    if (!codes) return err;
    int64_t nw = k == 2 ? 1 : k;
    LossJob j = {vm, x, codes, 1, 1, 0, m->p[P_FIT_INTERCEPT] != 0, 1 / (double)x->n, m->p[P_C] / (double)x->n, 1,
                 NULL, NULL, NULL};
    j.parts = (double *)alloc(sizeof(double) * (size_t)(blocks_of(x->n) * 2));
    double *w = (double *)alloc(sizeof(double) * (size_t)(nw * (d + 1)));
    int iters = 0;
    for (int64_t c = 0; c < nw; c++) {
        j.positive = k == 2 ? 1 : (int)c;
        int it = lbfgs(linear_loss, &j, w + c * (d + 1), d + 1, (int)m->p[P_MAX_ITER], m->p[P_TOL] * 1e-3);
        if (it > iters) iters = it;
    }
    set_linear_classes(m, w, nw, d);
    slot_number(m, S_N_ITER, iters, 1);
    free(j.parts);
    free(w);
    free(codes);
    return lube->nothing();
}

/* decision_function: n for two classes, n x k for more. */
static LubeValue decision_linear(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = m->s[S_COEF].shape[0];
    double *z = linear_scores(m, x), *out;
    LubeValue r = new_f64(vm, x->n, k == 1 ? -1 : k, &out);
    memcpy(out, z, sizeof(double) * (size_t)(x->n * k));
    free(z);
    return r;
}

static LubeValue predict_linear_class(LubeVM *vm, const Model *m, const Data *x) {
    int64_t k = m->s[S_COEF].shape[0];
    double *z = linear_scores(m, x);
    LubeValue r;
    if (k == 1) {
        int *codes = (int *)alloc(sizeof(int) * (size_t)x->n);
        for (int64_t i = 0; i < x->n; i++) codes[i] = z[i] > 0;
        r = labels_out(vm, m, codes, x->n);
        free(codes);
    } else r = argmax_labels(vm, m, z, x->n, k);
    free(z);
    return r;
}

/* The logistic model's probabilities, n x classes (log ones if logs). */
static LubeValue proba_logistic_as(LubeVM *vm, const Model *m, const Data *x, int logs) {
    int64_t k = m->s[S_COEF].shape[0], nc = k == 1 ? 2 : k;
    double *z = linear_scores(m, x), *p;
    LubeValue r = new_f64(vm, x->n, nc, &p);
    for (int64_t i = 0; i < x->n; i++) {
        double *pi = p + i * nc;
        if (k == 1) {  /* (log sigmoid(z) = -log(1 + e^-z)) */
            pi[1] = -log1pexp(-z[i]);
            pi[0] = -log1pexp(z[i]);
        } else {
            double top = z[i * k], sum = 0;
            for (int64_t c = 1; c < k; c++) top = fmax(top, z[i * k + c]);
            for (int64_t c = 0; c < k; c++) sum += exp(z[i * k + c] - top);
            for (int64_t c = 0; c < k; c++) pi[c] = z[i * k + c] - top - log(sum);
        }
        if (!logs)
            for (int64_t c = 0; c < nc; c++) pi[c] = exp(pi[c]);
    }
    free(z);
    return r;
}

static LubeValue proba_logistic(LubeVM *vm, const Model *m, const Data *x) { return proba_logistic_as(vm, m, x, 0); }
static LubeValue log_proba_logistic(LubeVM *vm, const Model *m, const Data *x) { return proba_logistic_as(vm, m, x, 1); }
