/*
 * learn/selection.c - choosing models: train_test_split, the folds of
 * KFold and StratifiedKFold (sklearn's), and cross_val_score.
 * (Part of learn.c.)
 */

/* How many samples (rows) v has: an Array's first dimension, or a list's length; -1 for anything else. */
static int64_t rows_of(LubeValue v) {
    Array *a = arr->get(v);
    if (a) return a->ndim ? a->shape[0] : -1;
    return lube->kind(v) == LUBE_LIST ? (int64_t)lube->list_len(v) : -1;
}

/* The rows idx[0..k) of v: an Array's (a copy), or a list's (a new list). */
static LubeValue take_rows(LubeVM *vm, LubeValue v, const int64_t *idx, int64_t k) {
    if (!arr->get(v)) {
        LubeValue list = lube->new_list(vm);
        for (int64_t i = 0; i < k; i++) lube->list_push(list, lube->retain(lube->list_get(v, (size_t)idx[i])));
        return list;
    }
    int64_t *iv;
    LubeValue at = new_i64(vm, k, -1, &iv), args[2] = {v, at};
    memcpy(iv, idx, sizeof(int64_t) * (size_t)k);
    LubeValue r = arr->index(vm, 2, args);
    lube->release(at);
    return r;
}

/* sklearn's _approximate_mode: n draws shared among classes in proportion to their counts, the remainders going
 * to the largest fractions (ties drawn at random). */
static void approximate_mode(Mt *rng, const double *counts, int64_t k, int64_t n, int64_t *out) {
    double total = 0, *rem = (double *)alloc(sizeof(double) * (size_t)k);
    int64_t given = 0, *ties = (int64_t *)alloc(sizeof(int64_t) * (size_t)k);
    for (int64_t c = 0; c < k; c++) total += counts[c];
    for (int64_t c = 0; c < k; c++) {
        double x = counts[c] / total * (double)n;
        out[c] = (int64_t)floor(x);
        rem[c] = x - floor(x);
        given += out[c];
    }
    while (given < n) {
        double top = -1;
        for (int64_t c = 0; c < k; c++) top = fmax(top, rem[c]);
        int64_t nt = 0;
        for (int64_t c = 0; c < k; c++)
            if (rem[c] == top) {
                ties[nt++] = c;
                rem[c] = -2;
            }
        mt_shuffle(rng, ties, nt);
        for (int64_t i = 0; i < nt && given < n; i++, given++) out[ties[i]]++;
    }
    free(rem);
    free(ties);
}

/* train_test_split(X, y..., options?): [X_train, X_test, y_train, y_test...]. Options: test_size, train_size
 * (shares below 1, or counts), shuffle (true), random_state, stratify (labels whose shares both sides keep). */
FN(f_train_test_split) {
    int na = argc;
    LubeValue opts = lube->nothing(), err, strat;
    if (lube->kind(argv[argc - 1]) == LUBE_OBJECT) {
        if (!options_in(vm, "train_test_split", argc, argv, argc - 1, "test_size|train_size|shuffle|random_state|stratify", &opts, &err))
            return err;
        na--;
    }
    if (na == 0) return lube->error(vm, "train_test_split() needs at least one Array (or list) to split");
    int64_t n = rows_of(argv[0]);
    for (int i = 0; i < na; i++) {
        int64_t r = rows_of(argv[i]);
        if (r < 0) return lube->error(vm, "train_test_split() expects Arrays or lists, got %s as argument %d", lube->a_type(argv[i]), i + 1);
        if (r != n) return lube->error(vm, "train_test_split(): argument %d has %.0f samples, but argument 1 has %.0f", i + 1, (double)r, (double)n);
    }
    double test, train, seed;
    int shuffle;
    LubeValue tv;
    int has_test = opt(vm, opts, "test_size", &tv), has_train = opt(vm, opts, "train_size", &tv);
    if (!opt_number(vm, "train_test_split", opts, "test_size", 0.25, 0, (double)n, 0, &test, &err) ||
        !opt_number(vm, "train_test_split", opts, "train_size", 0, 0, (double)n, 0, &train, &err) ||
        !opt_bool(vm, "train_test_split", opts, "shuffle", 1, &shuffle, &err) ||
        !opt_number(vm, "train_test_split", opts, "random_state", -1, 0, 4294967295.0, 1, &seed, &err))
        return err;
    int64_t n_test = test < 1 ? (int64_t)ceil(test * (double)n) : (int64_t)test;
    int64_t n_train = train < 1 ? (int64_t)floor(train * (double)n) : (int64_t)train;
    if (!has_train) n_train = n - n_test;
    else if (!has_test) n_test = n - n_train;
    if (n_train < 1 || n_test < 1 || n_train + n_test > n || (test >= 1 && test != floor(test)) || (train >= 1 && train != floor(train)))
        return lube->error(vm, "train_test_split(): %.0f samples cannot make %.0f to train on and %.0f to test", (double)n,
                           (double)n_train, (double)n_test);
    int64_t *idx = (int64_t *)alloc(sizeof(int64_t) * (size_t)n), *train_i = idx, *test_i = idx + n_train;
    Mt *rng = mt_for(seed);
    if (opt(vm, opts, "stratify", &strat)) {  /* (sklearn's StratifiedShuffleSplit) */
        Data y = NO_DATA;
        if (!data_in(vm, "train_test_split", "stratify", strat, argc - 1, W_VECTOR, &y, &err)) {
            free(idx);
            free(rng);
            return err;
        }
        if (y.n != n) {
            free(idx);
            free(rng);
            data_free(&y);
            return lube->error(vm, "train_test_split(): stratify has %.0f labels for %.0f samples", (double)y.n, (double)n);
        }
        int64_t k, at = 0, a = 0, b = 0;
        double *labels = distinct(y.v, n, &k), *counts = (double *)alloc(sizeof(double) * (size_t)k);
        int64_t *order = (int64_t *)alloc(sizeof(int64_t) * (size_t)n), *ni = (int64_t *)alloc(sizeof(int64_t) * (size_t)k);
        int64_t *ti = (int64_t *)alloc(sizeof(int64_t) * (size_t)k);
        for (int64_t i = 0; i < n; i++) counts[find_value(labels, k, y.v[i])]++;
        approximate_mode(rng, counts, k, n_train, ni);
        for (int64_t c = 0; c < k; c++) counts[c] -= (double)ni[c];
        approximate_mode(rng, counts, k, n_test, ti);
        for (int64_t c = 0; c < k; c++) {
            int64_t start = at;
            for (int64_t i = 0; i < n; i++)
                if (y.v[i] == labels[c]) order[at++] = i;
            mt_shuffle(rng, order + start, at - start);
            for (int64_t i = 0; i < ni[c]; i++) train_i[a++] = order[start + i];
            for (int64_t i = 0; i < ti[c]; i++) test_i[b++] = order[start + ni[c] + i];
        }
        mt_shuffle(rng, train_i, n_train);
        mt_shuffle(rng, test_i, n_test);
        free(order);
        free(ni);
        free(ti);
        free(labels);
        free(counts);
        data_free(&y);
    } else if (shuffle) {  /* a permutation: the test samples first, then the training ones */
        int64_t *perm = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
        for (int64_t i = 0; i < n; i++) perm[i] = i;
        mt_shuffle(rng, perm, n);
        memcpy(test_i, perm, sizeof(int64_t) * (size_t)n_test);
        memcpy(train_i, perm + n_test, sizeof(int64_t) * (size_t)n_train);
        free(perm);
    } else
        for (int64_t i = 0; i < n_train + n_test; i++) idx[i] = i;
    LubeValue out = lube->new_list(vm);
    for (int i = 0; i < na; i++) {
        lube->list_push(out, take_rows(vm, argv[i], train_i, n_train));
        lube->list_push(out, take_rows(vm, argv[i], test_i, n_test));
    }
    free(idx);
    free(rng);
    return out;
}

/* ======================================================================
 * Folds
 * ==================================================================== */

/* Each sample's fold (0..n_splits): KFold's runs of samples (shuffled first if asked), or StratifiedKFold's
 * classes dealt out among the folds. NULL with *err. */
static int *folds_of(LubeVM *vm, const char *fname, const Model *m, int64_t n, const Data *y, LubeValue *err) {
    int64_t k = (int64_t)m->p[P_N_SPLITS];
    if (k > n) {
        *err = lube->error(vm, "%s(): %s has n_splits=%.0f, more than the %.0f samples", fname, KIND_NAMES[m->kind], (double)k, (double)n);
        return NULL;
    }
    int *fold = (int *)alloc(sizeof(int) * (size_t)n);
    int shuffle = m->p[P_SHUFFLE] != 0;
    Mt *rng = shuffle ? mt_for(m->p[P_RANDOM_STATE]) : NULL;
    if (m->kind == K_KFOLD) {
        int64_t *idx = (int64_t *)alloc(sizeof(int64_t) * (size_t)n), at = 0;
        for (int64_t i = 0; i < n; i++) idx[i] = i;
        if (shuffle) mt_shuffle(rng, idx, n);
        for (int64_t f = 0; f < k; f++)
            for (int64_t c = 0; c < n / k + (f < n % k); c++) fold[idx[at++]] = (int)f;
        free(idx);
        free(rng);
        return fold;
    }
    /* sklearn's: classes numbered by where they first come, their sorted labels dealt round the folds (the i-th to
     * fold i mod k), and each class's samples, in order, given the folds its labels got (shuffled if asked) */
    int64_t nc, at = 0;
    double *labels = distinct(y->v, n, &nc);
    int64_t *rank = (int64_t *)alloc(sizeof(int64_t) * (size_t)nc), *code = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    int64_t *counts = (int64_t *)alloc(sizeof(int64_t) * (size_t)nc), *dealt = (int64_t *)alloc(sizeof(int64_t) * (size_t)(k * nc));
    for (int64_t c = 0; c < nc; c++) rank[c] = -1;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = find_value(labels, nc, y->v[i]);
        if (rank[c] < 0) rank[c] = at++;
        counts[code[i] = rank[c]]++;
    }
    for (int64_t c = 0, pos = 0; c < nc; c++)
        for (int64_t q = 0; q < counts[c]; q++, pos++) dealt[(pos % k) * nc + c]++;
    int64_t *ff = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    for (int64_t c = 0; c < nc; c++) {
        int64_t len = 0;
        for (int64_t f = 0; f < k; f++)
            for (int64_t q = 0; q < dealt[f * nc + c]; q++) ff[len++] = f;
        if (shuffle) mt_shuffle(rng, ff, len);
        for (int64_t i = 0, q = 0; i < n; i++)
            if (code[i] == c) fold[i] = (int)ff[q++];
    }
    free(labels);
    free(rank);
    free(code);
    free(counts);
    free(dealt);
    free(ff);
    free(rng);
    return fold;
}

/* The training and test samples of fold f (each in order): new arrays. */
static void fold_parts(const int *fold, int64_t n, int f, int64_t **train, int64_t *nt, int64_t **test, int64_t *ns) {
    *train = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    *test = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    *nt = *ns = 0;
    for (int64_t i = 0; i < n; i++) {
        if (fold[i] == f) (*test)[(*ns)++] = i;
        else (*train)[(*nt)++] = i;
    }
}

/* split(X, y?): a list of [train, test] index Arrays, one pair per fold. */
FN(m_split) {
    MODEL;
    int64_t n = rows_of(argv[1]);
    Data y = NO_DATA;
    LubeValue err;
    if (n < 0) return lube->error(vm, "split() expects an Array (or a list) as argument 1, got %s", lube->a_type(argv[1]));
    if (m->kind == K_STRATIFIED_KFOLD) {
        if (argc < 3) return lube->error(vm, "split() of StratifiedKFold needs X and y");
        if (!data_in(vm, "split", "y", argv[2], 1, W_VECTOR, &y, &err)) return err;
        if (y.n != n) {
            data_free(&y);
            return lube->error(vm, "split(): X has %.0f samples but y has %.0f", (double)n, (double)y.n);
        }
    }
    int *fold = folds_of(vm, "split", m, n, &y, &err);
    if (m->kind == K_STRATIFIED_KFOLD) data_free(&y);
    if (!fold) return err;
    LubeValue out = lube->new_list(vm);
    for (int f = 0; f < (int)m->p[P_N_SPLITS]; f++) {
        int64_t *train, *test, nt, ns, *a, *b;
        fold_parts(fold, n, f, &train, &nt, &test, &ns);
        LubeValue pair = lube->new_list(vm);
        lube->list_push(pair, new_i64(vm, nt, -1, &a));
        lube->list_push(pair, new_i64(vm, ns, -1, &b));
        memcpy(a, train, sizeof(int64_t) * (size_t)nt);
        memcpy(b, test, sizeof(int64_t) * (size_t)ns);
        lube->list_push(out, pair);
        free(train);
        free(test);
    }
    free(fold);
    return out;
}

FN(m_get_n_splits) {
    (void)argc;
    return lube->number_value(model_of(argv[0])->p[P_N_SPLITS]);
}

/* ======================================================================
 * cross_val_score
 * ==================================================================== */

/* Rows idx[0..k) of x, as Data of their own. */
static void data_rows(LubeVM *vm, const Data *x, const int64_t *idx, int64_t k, Data *out) {
    double *v;
    out->keep = new_f64(vm, k, x->nd == 2 ? x->d : -1, &v);
    for (int64_t i = 0; i < k; i++) memcpy(v + i * x->d, x->v + idx[i] * x->d, sizeof(double) * (size_t)x->d);
    out->v = v;
    out->n = k;
    out->d = x->d;
    out->nd = x->nd;
    out->dtype = x->dtype;
}

enum { SC_DEFAULT, SC_ACCURACY, SC_BALANCED, SC_F1, SC_F1_MACRO, SC_PRECISION, SC_RECALL, SC_R2, SC_NEG_MSE, SC_NEG_RMSE, SC_NEG_MAE };
#define SCORINGS "accuracy|balanced_accuracy|f1|f1_macro|precision|recall|r2|neg_mean_squared_error|neg_root_mean_squared_error|neg_mean_absolute_error"

/* The metric `scoring` names, of y and predictions p (Data of theirs). */
static LubeValue scoring_metric(LubeVM *vm, int scoring, LubeValue yv, LubeValue pv) {
    LubeValue args[3] = {yv, pv, lube->nothing()}, r;
    switch (scoring) {
    case SC_ACCURACY: return f_accuracy_score(vm, 2, args);
    case SC_BALANCED: return f_balanced_accuracy_score(vm, 2, args);
    case SC_F1: return f_f1_score(vm, 2, args);
    case SC_PRECISION: return f_precision_score(vm, 2, args);
    case SC_RECALL: return f_recall_score(vm, 2, args);
    case SC_R2: return f_r2_score(vm, 2, args);
    case SC_F1_MACRO:
        args[2] = lube->new_object(vm);
        lube->object_set(vm, args[2], "average", lube->new_str(vm, "macro", 5));
        r = f_f1_score(vm, 3, args);
        lube->release(args[2]);
        return r;
    default:
        r = scoring == SC_NEG_MSE ? f_mean_squared_error(vm, 2, args) : scoring == SC_NEG_RMSE ? f_root_mean_squared_error(vm, 2, args)
                                                                                                : f_mean_absolute_error(vm, 2, args);
        return lube->failed(r) ? r : lube->number_value(-lube->number(r));
    }
}

/* cross_val_score(model, X, y, {cv, scoring}): the model fitted on all folds but one and scored on that one, for
 * each fold: an Array of the scores. cv is the folds (5: StratifiedKFold for a classifier, else KFold) or a KFold
 * or StratifiedKFold; scoring is the model's score(), a metric's name, or fn(model, X, y). The model may also be
 * an object with fit(X, y) and score(X, y) methods of its own (fitted again for each fold: what fit returns is the
 * fitted model when it is an object, since an instance cannot change itself when a plugin calls it). */
FN(f_cross_val_score) {
    LubeValue opts, err, cv, scorer = lube->nothing();
    if (!options_in(vm, "cross_val_score", argc, argv, 3, "cv|scoring", &opts, &err)) return err;
    Model *m = model_of(argv[0]), *splitter = NULL;
    int scoring = SC_DEFAULT;
    if (opt(vm, opts, "scoring", &scorer)) {
        if (lube->kind(scorer) == LUBE_STR) {
            if ((scoring = opt_choice(vm, "cross_val_score", opts, "scoring", SCORINGS, 0, &err) + 1) == 0) return err;
        } else if (lube->kind(scorer) != LUBE_OTHER)
            return lube->error(vm, "cross_val_score(): scoring must be a metric's name or a function, not %s", lube->a_type(scorer));
    }
    if (!m && (lube->kind(argv[0]) != LUBE_OBJECT && lube->kind(argv[0]) != LUBE_FOREIGN))
        return lube->error(vm, "cross_val_score() expects a model as argument 1, got %s", lube->a_type(argv[0]));
    if (m && (KINDS[m->kind].ywant < 0 && m->kind != K_KMEANS))
        return lube->error(vm, "cross_val_score(): a %s cannot be scored", KIND_NAMES[m->kind]);
    Data x = NO_DATA, y = NO_DATA;
    if (!xy_in(vm, "cross_val_score", argv[1], argv[2], W_EITHER, &x, &y, &err)) return err;
    Model folds_model;
    if (opt(vm, opts, "cv", &cv) && lube->kind(cv) != LUBE_NUMBER) {
        splitter = model_of(cv);
        if (!splitter || KINDS[splitter->kind].role != R_SPLITTER) {
            data_free(&x);
            data_free(&y);
            return lube->error(vm, "cross_val_score(): cv must be a number of folds, a KFold or a StratifiedKFold, not %s", lube->a_type(cv));
        }
    } else {
        double k;
        if (!opt_number(vm, "cross_val_score", opts, "cv", 5, 2, (double)x.n, 1, &k, &err)) {
            data_free(&x);
            data_free(&y);
            return err;
        }
        memset(&folds_model, 0, sizeof folds_model);
        folds_model.kind = m && KINDS[m->kind].role == R_CLASSIFIER ? K_STRATIFIED_KFOLD : K_KFOLD;
        folds_model.p[P_N_SPLITS] = k;
        folds_model.p[P_RANDOM_STATE] = -1;
        splitter = &folds_model;
    }
    int *fold = folds_of(vm, "cross_val_score", splitter, x.n, &y, &err);
    if (!fold) {
        data_free(&x);
        data_free(&y);
        return err;
    }
    int nf = (int)splitter->p[P_N_SPLITS];
    double *scores;
    LubeValue out = new_f64(vm, nf, -1, &scores), r = out;
    for (int f = 0; f < nf; f++) {
        int64_t *train, *test, nt, ns;
        fold_parts(fold, x.n, f, &train, &nt, &test, &ns);
        Data xa, ya, xb, yb;  /* (data_rows sets them) */
        data_rows(vm, &x, train, nt, &xa);
        data_rows(vm, &y, train, nt, &ya);
        data_rows(vm, &x, test, ns, &xb);
        data_rows(vm, &y, test, ns, &yb);
        LubeValue fitted, res = lube->nothing();
        if (m) {
            Model *c;
            fitted = model_clone(vm, m, &c);
            res = fit_on(vm, c, &xa, KINDS[c->kind].ywant >= 0 ? &ya : NULL);
            if (!lube->failed(res) && scoring == SC_DEFAULT && lube->kind(scorer) != LUBE_OTHER) {
                if (score_on(vm, c, &xb, &yb, &scores[f], &err)) res = lube->nothing();
                else res = err;
                goto next;
            }
        } else {  /* (fit may return the fitted model, as an instance can't change itself here: else it is the model) */
            LubeValue args[2] = {xa.keep, ya.keep};
            res = lube->call_method(vm, argv[0], "fit", 2, args);
            int made = !lube->failed(res) && (lube->kind(res) == LUBE_OBJECT || lube->kind(res) == LUBE_FOREIGN);
            fitted = made ? res : lube->retain(argv[0]);
            if (made) res = lube->nothing();
            if (!lube->failed(res) && scoring == SC_DEFAULT && lube->kind(scorer) != LUBE_OTHER) {
                LubeValue sargs[2] = {xb.keep, yb.keep};
                lube->release(res);
                res = lube->call_method(vm, fitted, "score", 2, sargs);
                goto got;
            }
        }
        if (!lube->failed(res)) {
            lube->release(res);
            if (lube->kind(scorer) == LUBE_OTHER) {
                LubeValue args[3] = {fitted, xb.keep, yb.keep};
                res = lube->call(vm, scorer, 3, args);
            } else {
                LubeValue args[1] = {xb.keep}, pred = lube->call_method(vm, fitted, "predict", 1, args);
                res = lube->failed(pred) ? pred : scoring_metric(vm, scoring, yb.keep, pred);
                if (!lube->failed(pred)) lube->release(pred);
            }
        }
    got:
        if (!lube->failed(res)) {
            if (lube->kind(res) != LUBE_NUMBER) {
                char got[128];
                LubeValue bad = lube->error(vm, "cross_val_score(): a score must be a Number, not %s", value_text(res, got, sizeof got));
                lube->release(res);
                res = bad;
            } else {
                scores[f] = lube->number(res);
                lube->release(res);
                res = lube->nothing();
            }
        }
    next:
        lube->release(fitted);
        data_free(&xa);
        data_free(&ya);
        data_free(&xb);
        data_free(&yb);
        free(train);
        free(test);
        if (lube->failed(res)) {
            lube->release(out);
            r = res;
            break;
        }
    }
    free(fold);
    data_free(&x);
    data_free(&y);
    return r;
}
