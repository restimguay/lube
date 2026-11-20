/*
 * learn/models.c - the models as Lube types: each kind's options and
 * defaults, what it can do (fit, predict, transform...), its properties,
 * and the methods all of them share. (Part of learn.c.)
 */

typedef LubeValue (*FitFn)(LubeVM *vm, Model *m, const Data *x, const Data *y);  /* nothing, or an error */
typedef LubeValue (*OutFn)(LubeVM *vm, const Model *m, const Data *x);          /* an Array, or an error */

typedef struct { short id; double def; } KParam;

enum { R_CLASSIFIER, R_REGRESSOR, R_CLUSTER, R_TRANSFORMER, R_SPLITTER };

typedef struct {
    int role;
    int ywant;               /* fit's y (W_VECTOR or W_EITHER), or -1: no y */
    int positional;          /* the first option may come as a Number: Ridge(0.5) */
    const KParam *params;    /* options and their defaults, up to {-1} */
    const signed char *props;  /* slots shown as properties, up to -1 */
    FitFn fit;
    OutFn predict, proba, log_proba, decision, transform, inverse;
} Kind;

#define END {-1, 0}
static const KParam KP_LINEAR[] = {{P_FIT_INTERCEPT, 1}, END};
static const KParam KP_RIDGE[] = {{P_ALPHA, 1}, {P_FIT_INTERCEPT, 1}, END};
static const KParam KP_LASSO[] = {{P_ALPHA, 1}, {P_FIT_INTERCEPT, 1}, {P_MAX_ITER, 1000}, {P_TOL, 1e-4}, END};
static const KParam KP_ENET[] = {{P_ALPHA, 1}, {P_L1_RATIO, 0.5}, {P_FIT_INTERCEPT, 1}, {P_MAX_ITER, 1000}, {P_TOL, 1e-4}, END};
static const KParam KP_LOGISTIC[] = {{P_C, 1}, {P_FIT_INTERCEPT, 1}, {P_MAX_ITER, 100}, {P_TOL, 1e-4}, END};
static const KParam KP_SVC[] = {{P_C, 1}, {P_FIT_INTERCEPT, 1}, {P_MAX_ITER, 1000}, {P_TOL, 1e-4}, END};
static const KParam KP_KNN[] = {{P_N_NEIGHBORS, 5}, {P_WEIGHTS, 0}, {P_P, 2}, END};
static const KParam KP_TREE_C[] = {{P_CRITERION, 0}, {P_MAX_DEPTH, -1}, {P_MIN_SAMPLES_SPLIT, 2}, {P_MIN_SAMPLES_LEAF, 1},
                                   {P_MAX_FEATURES, -1}, {P_RANDOM_STATE, -1}, END};
static const KParam KP_TREE_R[] = {{P_CRITERION_R, 0}, {P_MAX_DEPTH, -1}, {P_MIN_SAMPLES_SPLIT, 2}, {P_MIN_SAMPLES_LEAF, 1},
                                   {P_MAX_FEATURES, -1}, {P_RANDOM_STATE, -1}, END};
static const KParam KP_FOREST_C[] = {{P_N_ESTIMATORS, 100}, {P_CRITERION, 0}, {P_MAX_DEPTH, -1}, {P_MIN_SAMPLES_SPLIT, 2},
                                     {P_MIN_SAMPLES_LEAF, 1}, {P_MAX_FEATURES, -2}, {P_BOOTSTRAP, 1}, {P_RANDOM_STATE, -1}, END};
static const KParam KP_FOREST_R[] = {{P_N_ESTIMATORS, 100}, {P_CRITERION_R, 0}, {P_MAX_DEPTH, -1}, {P_MIN_SAMPLES_SPLIT, 2},
                                     {P_MIN_SAMPLES_LEAF, 1}, {P_MAX_FEATURES, -1}, {P_BOOTSTRAP, 1}, {P_RANDOM_STATE, -1}, END};
static const KParam KP_NB[] = {{P_VAR_SMOOTHING, 1e-9}, END};
static const KParam KP_KMEANS[] = {{P_N_CLUSTERS, 8}, {P_INIT, 0}, {P_N_INIT, -1}, {P_MAX_ITER, 300}, {P_TOL, 1e-4},
                                   {P_RANDOM_STATE, -1}, END};
static const KParam KP_DBSCAN[] = {{P_EPS, 0.5}, {P_MIN_SAMPLES, 5}, END};
static const KParam KP_PCA[] = {{P_N_COMPONENTS, -1}, {P_WHITEN, 0}, END};
static const KParam KP_STANDARD[] = {{P_WITH_MEAN, 1}, {P_WITH_STD, 1}, END};
static const KParam KP_MINMAX[] = {{P_FEATURE_RANGE, 0}, {P_RANGE_HI, 1}, {P_CLIP, 0}, END};
static const KParam KP_NONE[] = {END};
static const KParam KP_ONEHOT[] = {{P_HANDLE_UNKNOWN, 0}, {P_DROP, 0}, END};
static const KParam KP_POLY[] = {{P_DEGREE, 2}, {P_INTERACTION_ONLY, 0}, {P_INCLUDE_BIAS, 1}, END};
static const KParam KP_KFOLD[] = {{P_N_SPLITS, 5}, {P_SHUFFLE, 0}, {P_RANDOM_STATE, -1}, END};

static const signed char PR_LINEAR[] = {S_COEF, S_INTERCEPT, S_N_FEATURES, -1};
static const signed char PR_ENET[] = {S_COEF, S_INTERCEPT, S_N_ITER, S_N_FEATURES, -1};
static const signed char PR_LINEAR_C[] = {S_COEF, S_INTERCEPT, S_CLASSES, S_N_ITER, S_N_FEATURES, -1};
static const signed char PR_CLASSES[] = {S_CLASSES, S_N_FEATURES, -1};
static const signed char PR_FEATURES[] = {S_N_FEATURES, -1};
static const signed char PR_TREE_C[] = {S_CLASSES, S_IMPORTANCES, S_N_FEATURES, -1};
static const signed char PR_TREE_R[] = {S_IMPORTANCES, S_N_FEATURES, -1};
static const signed char PR_NB[] = {S_CLASSES, S_THETA, S_VAR, S_PRIOR, S_COUNT, S_EPSILON, S_N_FEATURES, -1};
static const signed char PR_KMEANS[] = {S_CENTERS, S_LABELS, S_INERTIA, S_N_ITER, S_N_FEATURES, -1};
static const signed char PR_DBSCAN[] = {S_LABELS, S_CORE, S_COMPONENTS, S_N_FEATURES, -1};
static const signed char PR_PCA[] = {S_COMPONENTS, S_EVAR, S_EVAR_RATIO, S_SINGULAR, S_MEAN, S_N_COMPONENTS, S_NOISE,
                                     S_N_FEATURES, -1};
static const signed char PR_STANDARD[] = {S_MEAN, S_SCALE, S_VAR, S_N_SEEN, S_N_FEATURES, -1};
static const signed char PR_MINMAX[] = {S_MIN, S_SCALE, S_DATA_MIN, S_DATA_MAX, S_DATA_RANGE, S_N_SEEN, S_N_FEATURES, -1};
static const signed char PR_LABELS[] = {S_CLASSES, -1};
static const signed char PR_ONEHOT[] = {S_N_FEATURES, -1};
static const signed char PR_POLY[] = {S_POWERS, S_N_OUT, S_N_FEATURES, -1};
static const signed char PR_NONE[] = {-1};

static const Kind KINDS[NKINDS] = {
    {R_REGRESSOR, W_EITHER, 0, KP_LINEAR, PR_LINEAR, fit_least_squares, predict_linear, NULL, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_EITHER, 1, KP_RIDGE, PR_LINEAR, fit_least_squares, predict_linear, NULL, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_EITHER, 1, KP_LASSO, PR_ENET, fit_enet, predict_linear, NULL, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_EITHER, 1, KP_ENET, PR_ENET, fit_enet, predict_linear, NULL, NULL, NULL, NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 0, KP_LOGISTIC, PR_LINEAR_C, fit_logistic, predict_linear_class, proba_logistic,
     log_proba_logistic, decision_linear, NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 0, KP_SVC, PR_LINEAR_C, fit_linear_svc, predict_linear_class, NULL, NULL, decision_linear,
     NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 1, KP_KNN, PR_CLASSES, fit_knn, predict_knn_class, proba_knn, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_EITHER, 1, KP_KNN, PR_FEATURES, fit_knn, predict_knn_reg, NULL, NULL, NULL, NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 0, KP_TREE_C, PR_TREE_C, fit_tree, predict_tree, proba_tree, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_VECTOR, 0, KP_TREE_R, PR_TREE_R, fit_tree, predict_tree, NULL, NULL, NULL, NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 1, KP_FOREST_C, PR_TREE_C, fit_tree, predict_tree, proba_tree, NULL, NULL, NULL, NULL},
    {R_REGRESSOR, W_VECTOR, 1, KP_FOREST_R, PR_TREE_R, fit_tree, predict_tree, NULL, NULL, NULL, NULL, NULL},
    {R_CLASSIFIER, W_VECTOR, 0, KP_NB, PR_NB, fit_gaussian_nb, predict_nb, proba_nb, log_proba_nb, NULL, NULL, NULL},
    {R_CLUSTER, -1, 1, KP_KMEANS, PR_KMEANS, fit_kmeans, predict_kmeans, NULL, NULL, NULL, transform_kmeans, NULL},
    {R_CLUSTER, -1, 1, KP_DBSCAN, PR_DBSCAN, fit_dbscan, NULL, NULL, NULL, NULL, NULL, NULL},
    {R_TRANSFORMER, -1, 1, KP_PCA, PR_PCA, fit_pca, NULL, NULL, NULL, NULL, transform_pca, inverse_pca},
    {R_TRANSFORMER, -1, 0, KP_STANDARD, PR_STANDARD, fit_standard, NULL, NULL, NULL, NULL, transform_scale, inverse_scale},
    {R_TRANSFORMER, -1, 0, KP_MINMAX, PR_MINMAX, fit_minmax, NULL, NULL, NULL, NULL, transform_scale, inverse_scale},
    {R_TRANSFORMER, -1, 0, KP_NONE, PR_LABELS, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {R_TRANSFORMER, -1, 0, KP_ONEHOT, PR_ONEHOT, fit_onehot, NULL, NULL, NULL, NULL, transform_onehot, inverse_onehot},
    {R_TRANSFORMER, -1, 1, KP_POLY, PR_POLY, fit_poly, NULL, NULL, NULL, NULL, transform_poly, NULL},
    {R_SPLITTER, -1, 1, KP_KFOLD, PR_NONE, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {R_SPLITTER, -1, 1, KP_KFOLD, PR_NONE, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

static LubeType *TYPES[NKINDS];  /* (set once, in lube_plugin_init) */

/* v's model (v is one: a method's argv[0]). */
static Model *model_of(LubeValue v) {
    for (int k = 0; k < NKINDS; k++) {
        Model *m = (Model *)lube->foreign(v, TYPES[k]);
        if (m) return m;
    }
    return NULL;
}

static void model_release(void *data) { model_reset((Model *)data); }

/* ======================================================================
 * Options: from an object, checked; printed as sklearn prints them
 * ==================================================================== */

/* How option id's value v prints (into buf). */
static void option_text(const Model *m, int id, char *buf, size_t size) {
    double v = m->p[id];
    char num[32], c[32], hi[32];  /* (a Number's text fits in 32) */
    lube->format_number(v, num, sizeof num);
    switch (PARAMS[id].type) {
    case T_BOOL: snprintf(buf, size, "%s", v != 0 ? "true" : "false"); break;
    case T_CHOICE: snprintf(buf, size, "\"%s\"", choice_name(id, (int)v, c, sizeof c)); break;
    case T_FEATURES:
        if (v < 0) snprintf(buf, size, "\"%s\"", choice_name(id, v == -2 ? 0 : v == -3 ? 1 : 2, c, sizeof c));
        else snprintf(buf, size, "%s", num);
        break;
    case T_AUTO:
        snprintf(buf, size, "%s", v == -1 ? "\"auto\"" : num);
        break;
    case T_RANGE:
        lube->format_number(m->p[id + 1], hi, sizeof hi);
        snprintf(buf, size, "[%s, %s]", num, hi);
        break;
    default: snprintf(buf, size, "%s", num);
    }
}

static int model_describe(const void *data, char *buf, size_t size) {
    const Model *m = (const Model *)data;
    size_t at = (size_t)snprintf(buf, size, "%s(", KIND_NAMES[m->kind]);
    int first = 1;
    for (const KParam *kp = KINDS[m->kind].params; kp->id >= 0; kp++) {
        if (!m->given[kp->id] || !*PARAMS[kp->id].name) continue;
        char text[128];
        option_text(m, kp->id, text, sizeof text);
        at += (size_t)snprintf(buf + (at < size ? at : size), at < size ? size - at : 0, "%s%s=%s", first ? "" : ", ",
                               PARAMS[kp->id].name, text);
        first = 0;
    }
    return (int)at + snprintf(buf + (at < size ? at : size), at < size ? size - at : 0, ")");
}

/* What a value is, for messages: a Number or a str as it is, else its type. */
static const char *value_text(LubeValue v, char *buf, size_t size) {
    if (lube->kind(v) == LUBE_NUMBER) lube->format_number(lube->number(v), buf, size);
    else if (lube->kind(v) == LUBE_STR) snprintf(buf, size, "\"%s\"", lube->str(v, NULL));
    else snprintf(buf, size, "%s", lube->a_type(v));
    return buf;
}

/* Sets option id of m from v: 0 with *err if v is not a value it takes. */
static int set_option(LubeVM *vm, const char *fname, Model *m, int id, LubeValue v, LubeValue *err) {
    int k = lube->kind(v), t = PARAMS[id].type;
    double x = lube->number(v), lo = PARAMS[id].lo, hi = PARAMS[id].hi;
    int whole = k == LUBE_NUMBER && x == floor(x);
    const char *want = NULL;
    char wbuf[128];
    switch (t) {
    case T_NUM:
        if (k != LUBE_NUMBER || !(x >= lo && x <= hi)) {
            if (hi == INFINITY) snprintf(wbuf, sizeof wbuf, "a Number, %g or more", lo);
            else snprintf(wbuf, sizeof wbuf, "a Number from %g to %g", lo, hi);
            want = wbuf;
        }
        break;
    case T_POS:
        if (k != LUBE_NUMBER || !(x > 0) || x == INFINITY) want = "a Number more than 0";
        break;
    case T_WHOLE:
    case T_SEED:
    case T_DEPTH:
        if (!whole || x < lo || x > hi) {
            if (hi >= 1e9 && t != T_SEED) snprintf(wbuf, sizeof wbuf, "a whole Number, %g or more", lo);
            else snprintf(wbuf, sizeof wbuf, "a whole Number from %g to %.0f", lo, hi);
            want = wbuf;
        }
        break;
    case T_BOOL:
        if (k != LUBE_BOOL) want = "true or false";
        else x = lube->truthy(v);
        break;
    case T_CHOICE:
    case T_FEATURES: {
        const char *s = lube->str(v, NULL);
        int c = s ? choice_index(id, s) : -1;
        if (t == T_FEATURES && whole && x >= 1) break;
        if (t == T_FEATURES && k == LUBE_NUMBER && x > 0 && x < 1) break;
        if (c < 0) {
            size_t at = (size_t)snprintf(wbuf, sizeof wbuf, "%s", t == T_FEATURES ? "a count, a share below 1, " : "");
            int count = 1;
            for (const char *p = PARAMS[id].choices; *p; p++) count += *p == '|';
            for (int i = 0; i < count; i++) {
                char cn[32];
                at += (size_t)snprintf(wbuf + at, sizeof wbuf - at, "%s\"%s\"", i == 0 ? "" : i == count - 1 ? " or " : ", ",
                                       choice_name(id, i, cn, sizeof cn));
            }
            want = wbuf;
        } else x = t == T_FEATURES ? -2 - c : c;
        if (t == T_FEATURES && x == -4) x = -1;  /* ("all") */
        break;
    }
    case T_AUTO:
        if (k == LUBE_STR && !strcmp(lube->str(v, NULL), "auto")) x = -1;
        else if (!whole || x < lo || x > hi) want = "a whole Number, 1 or more, or \"auto\"";
        break;
    case T_COMPONENTS:
        if (!(whole && x >= 1) && !(k == LUBE_NUMBER && x > 0 && x < 1)) want = "a whole Number, 1 or more, or a share below 1";
        break;
    case T_RANGE: {
        double a = 0, b = 0;
        int ok = k == LUBE_LIST && lube->list_len(v) == 2 && lube->kind(lube->list_get(v, 0)) == LUBE_NUMBER &&
                 lube->kind(lube->list_get(v, 1)) == LUBE_NUMBER;
        if (ok) {
            a = lube->number(lube->list_get(v, 0));
            b = lube->number(lube->list_get(v, 1));
        }
        if (!ok || !(a < b)) want = "a list [low, high] with low below high";
        else {
            x = a;
            m->p[id + 1] = b;
        }
        break;
    }
    }
    if (want) {
        char got[128];
        *err = lube->error(vm, "%s(): %s must be %s, not %s", fname, PARAMS[id].name, want, value_text(v, got, sizeof got));
        return 0;
    }
    m->p[id] = x;
    m->given[id] = 1;
    return 1;
}

/* Sets m's options from the object opts: 0 with *err for an option it does not have, or a value it cannot take. */
static int set_options(LubeVM *vm, const char *fname, Model *m, LubeValue opts, LubeValue *err) {
    LubeValue keys = lube->object_keys(vm, opts);
    int ok = 1;
    for (size_t i = 0; ok && i < lube->list_len(keys); i++) {
        const char *key = lube->str(lube->list_get(keys, i), NULL);
        const KParam *kp = KINDS[m->kind].params;
        while (kp->id >= 0 && (!*PARAMS[kp->id].name || strcmp(PARAMS[kp->id].name, key))) kp++;
        if (kp->id < 0) {
            char names[512] = "";
            size_t at = 0;
            for (kp = KINDS[m->kind].params; kp->id >= 0; kp++)
                if (*PARAMS[kp->id].name)
                    at += (size_t)snprintf(names + at, sizeof names - at, "%s%s", at ? ", " : "", PARAMS[kp->id].name);
            *err = lube->error(vm, "%s(): there is no option \"%s\" (%s%s)", fname, key,
                               at ? "it has " : "it has no options", names);
            ok = 0;
        } else {
            LubeValue v;
            lube->object_get(vm, opts, key, &v);
            ok = set_option(vm, fname, m, kp->id, v, err);
        }
    }
    lube->release(keys);
    return ok;
}

/* A new model of kind k with its defaults: *out its data. */
static LubeValue model_new(LubeVM *vm, int k, Model **out) {
    void *data;
    LubeValue v = lube->new_foreign(vm, TYPES[k], &data);
    Model *m = (Model *)data;
    m->kind = k;
    for (int i = 0; i < NPARAMS; i++) m->p[i] = -1;
    for (const KParam *kp = KINDS[k].params; kp->id >= 0; kp++) m->p[kp->id] = kp->def;
    m->label_dtype = ARR_F64;
    *out = m;
    return v;
}

/* Kind(options?), or Kind(first option, options?) for the kinds sklearn takes one so. */
static LubeValue construct(LubeVM *vm, int k, int argc, const LubeValue *argv) {
    Model *m;
    LubeValue v = model_new(vm, k, &m), err;
    int i = 0;
    const char *name = KIND_NAMES[k];
    if (argc > 0 && KINDS[k].positional && lube->kind(argv[0]) == LUBE_NUMBER) {
        if (!set_option(vm, name, m, KINDS[k].params[0].id, argv[0], &err)) goto fail;
        i = 1;
    }
    if (argc > i) {
        if (lube->kind(argv[i]) != LUBE_OBJECT) {
            err = lube->error(vm, "%s() takes its options as an object, such as %s({%s: ...}); got %s", name, name,
                              KINDS[k].params[0].id >= 0 ? PARAMS[KINDS[k].params[0].id].name : "", lube->a_type(argv[i]));
            goto fail;
        }
        if (!set_options(vm, name, m, argv[i], &err)) goto fail;
        i++;
    }
    if (argc > i) {
        err = lube->error(vm, "%s() takes options as one object, then nothing more", name);
        goto fail;
    }
    return v;
fail:
    lube->release(v);
    return err;
}

/* A model of m's kind with m's options, not fitted (sklearn's clone). */
static LubeValue model_clone(LubeVM *vm, const Model *m, Model **out) {
    LubeValue v = model_new(vm, m->kind, out);
    memcpy((*out)->p, m->p, sizeof m->p);
    memcpy((*out)->given, m->given, sizeof m->given);
    return v;
}

/* ======================================================================
 * The methods all models share
 * ==================================================================== */

#define MODEL Model *m = model_of(argv[0])
#define FNAME (__func__ + 2)  /* "m_fit" -> "fit" */

static LubeValue not_fitted(LubeVM *vm, const char *fname, const Model *m) {
    return lube->error(vm, "%s(): this %s is not fitted yet: call fit() first", fname, KIND_NAMES[m->kind]);
}

/* m fitted on X and y (y NULL for none): nothing, or an error (m then unfitted). */
static LubeValue fit_on(LubeVM *vm, Model *m, const Data *x, const Data *y) {
    model_reset(m);
    LubeValue r = KINDS[m->kind].fit(vm, m, x, y);
    if (lube->failed(r)) {
        model_reset(m);
        return r;
    }
    m->fitted = 1;
    m->n_features = x->d;
    slot_number(m, S_N_FEATURES, (double)x->d, 1);
    return r;
}

/* fit(X, y) for the models that learn from labels, fit(X) for the others (a y is then left alone). */
FN(m_fit) {
    MODEL;
    const Kind *k = &KINDS[m->kind];
    if (k->ywant >= 0 && argc < 3) return lube->error(vm, "fit() of %s needs X and y", KIND_NAMES[m->kind]);
    Data x = NO_DATA, y = NO_DATA;
    LubeValue err;
    if (k->ywant >= 0 ? !xy_in(vm, "fit", argv[1], argv[2], k->ywant, &x, &y, &err)
                      : !data_in(vm, "fit", "X", argv[1], 0, W_MATRIX, &x, &err))
        return err;
    LubeValue r = fit_on(vm, m, &x, k->ywant >= 0 ? &y : NULL);
    data_free(&x);
    if (k->ywant >= 0) data_free(&y);
    return lube->failed(r) ? r : lube->retain(argv[0]);
}

/* X for a fitted model's fname: 0 with *err if m is not fitted or X has other features than fit's. */
static int x_for(LubeVM *vm, const char *fname, const Model *m, LubeValue v, Data *x, LubeValue *err) {
    if (!m->fitted) {
        *err = not_fitted(vm, fname, m);
        return 0;
    }
    if (!data_in(vm, fname, "X", v, 0, W_MATRIX, x, err)) return 0;
    if (x->d != m->n_features) {
        *err = lube->error(vm, "%s(): X has %.0f features, but %s was fitted with %.0f", fname, (double)x->d,
                           KIND_NAMES[m->kind], (double)m->n_features);
        data_free(x);
        return 0;
    }
    return 1;
}

static LubeValue apply_out(LubeVM *vm, const char *fname, LubeValue self, LubeValue xv, OutFn fn) {
    Model *m = model_of(self);
    Data x = NO_DATA;
    LubeValue err;
    if (!x_for(vm, fname, m, xv, &x, &err)) return err;
    LubeValue r = fn(vm, m, &x);
    data_free(&x);
    return r;
}

FN(m_predict) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], KINDS[model_of(argv[0])->kind].predict); }
FN(m_predict_proba) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], KINDS[model_of(argv[0])->kind].proba); }
FN(m_predict_log_proba) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], KINDS[model_of(argv[0])->kind].log_proba); }
FN(m_decision_function) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], KINDS[model_of(argv[0])->kind].decision); }
FN(m_transform) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], KINDS[model_of(argv[0])->kind].transform); }
FN(m_apply) { (void)argc; return apply_out(vm, FNAME, argv[0], argv[1], tree_apply); }

FN(m_inverse_transform) {
    (void)argc;
    MODEL;
    Data x = NO_DATA;
    LubeValue err;
    if (!m->fitted) return not_fitted(vm, FNAME, m);
    if (!data_in(vm, FNAME, "X", argv[1], 0, W_MATRIX, &x, &err)) return err;
    LubeValue r = KINDS[m->kind].inverse(vm, m, &x);
    data_free(&x);
    return r;
}

/* fit(X), then what transform(X) (or, for clusters, the labels) gives. */
static LubeValue fit_then(LubeVM *vm, LubeValue self, LubeValue xv, int labels) {
    Model *m = model_of(self);
    Data x = NO_DATA;
    LubeValue err;
    if (!data_in(vm, labels ? "fit_predict" : "fit_transform", "X", xv, 0, W_MATRIX, &x, &err)) return err;
    LubeValue r = fit_on(vm, m, &x, NULL);
    if (!lube->failed(r)) r = labels ? slot_out(vm, &m->s[S_LABELS]) : KINDS[m->kind].transform(vm, m, &x);
    data_free(&x);
    return r;
}

FN(m_fit_transform) { (void)argc; return fit_then(vm, argv[0], argv[1], 0); }
FN(m_fit_predict) { (void)argc; return fit_then(vm, argv[0], argv[1], 1); }

/* The share of y that pred gets right. */
static double accuracy_of(const double *y, const double *pred, int64_t n) {
    int64_t right = 0;
    for (int64_t i = 0; i < n; i++) right += y[i] == pred[i];
    return (double)right / (double)n;
}

/* R^2 of pred for y (n x t: the mean over the targets). */
static double r2_of(const double *y, const double *pred, int64_t n, int64_t t) {
    double total = 0;
    for (int64_t o = 0; o < t; o++) {
        double mean = 0, ss_res = 0, ss_tot = 0;
        for (int64_t i = 0; i < n; i++) mean += y[i * t + o];
        mean /= (double)n;
        for (int64_t i = 0; i < n; i++) {
            ss_res += (y[i * t + o] - pred[i * t + o]) * (y[i * t + o] - pred[i * t + o]);
            ss_tot += (y[i * t + o] - mean) * (y[i * t + o] - mean);
        }
        total += ss_tot == 0 ? (ss_res == 0 ? 1.0 : 0.0) : 1 - ss_res / ss_tot;
    }
    return total / (double)t;
}

/* m's score on X and y: accuracy (classifiers), R^2 (regressors), minus the inertia (KMeans). 0 with *err. */
static int score_on(LubeVM *vm, const Model *m, const Data *x, const Data *y, double *out, LubeValue *err) {
    if (m->kind == K_KMEANS) {
        free(kmeans_labels(m, x, out));
        *out = -*out;
        return 1;
    }
    LubeValue p = KINDS[m->kind].predict(vm, m, x);
    if (lube->failed(p)) {
        *err = p;
        return 0;
    }
    LubeValue pc = arr->contiguous(vm, p, ARR_F64);
    const Array *pa = arr->get(pc);
    if (pa->size != y->n * y->d) {
        *err = lube->error(vm, "score(): y has %.0f values, but there are %.0f predictions", (double)(y->n * y->d), (double)pa->size);
        lube->release(p);
        lube->release(pc);
        return 0;
    }
    *out = KINDS[m->kind].role == R_CLASSIFIER ? accuracy_of(y->v, (const double *)pa->data, y->n)
                                               : r2_of(y->v, (const double *)pa->data, y->n, y->d);
    lube->release(p);
    lube->release(pc);
    return 1;
}

FN(m_score) {
    MODEL;
    Data x = NO_DATA, y = NO_DATA;
    LubeValue err;
    int needs_y = m->kind != K_KMEANS;
    if (needs_y && argc < 3) return lube->error(vm, "score() of %s needs X and y", KIND_NAMES[m->kind]);
    if (!x_for(vm, "score", m, argv[1], &x, &err)) return err;
    if (needs_y && !data_in(vm, "score", "y", argv[2], 1, KINDS[m->kind].ywant, &y, &err)) {
        data_free(&x);
        return err;
    }
    double s;
    int ok = score_on(vm, m, &x, needs_y ? &y : NULL, &s, &err);
    data_free(&x);
    if (needs_y) data_free(&y);
    return ok ? lube->number_value(s) : err;
}

/* get_params(): the options, as an object (those that are not set, such as a max_depth of none, left out). */
FN(m_get_params) {
    (void)argc;
    MODEL;
    LubeValue o = lube->new_object(vm);
    for (const KParam *kp = KINDS[m->kind].params; kp->id >= 0; kp++) {
        int id = kp->id, t = PARAMS[id].type;
        double v = m->p[id];
        char c[32];
        if (!*PARAMS[id].name || ((t == T_SEED || t == T_DEPTH || t == T_COMPONENTS || t == T_FEATURES) && v == -1)) continue;
        LubeValue x;
        if (t == T_BOOL) x = lube->boolean(v != 0);
        else if (t == T_AUTO && v == -1) x = lube->new_str(vm, "auto", 4);
        else if (t == T_CHOICE) x = lube->new_str(vm, choice_name(id, (int)v, c, sizeof c), strlen(c));
        else if (t == T_FEATURES && v < 0) x = lube->new_str(vm, choice_name(id, v == -2 ? 0 : 1, c, sizeof c), strlen(c));
        else if (t == T_RANGE) {
            x = lube->new_list(vm);
            lube->list_push(x, lube->number_value(v));
            lube->list_push(x, lube->number_value(m->p[id + 1]));
        } else x = lube->number_value(v);
        lube->object_set(vm, o, PARAMS[id].name, x);
    }
    return o;
}

/* set_params(options): changes options (as the constructor takes them); returns the model. */
FN(m_set_params) {
    (void)argc;
    MODEL;
    LubeValue err;
    if (lube->kind(argv[1]) != LUBE_OBJECT) return lube->error(vm, "set_params() expects an object, got %s", lube->a_type(argv[1]));
    if (!set_options(vm, "set_params", m, argv[1], &err)) return err;
    return lube->retain(argv[0]);
}

/* ---------- some kinds' own ---------- */

/* kneighbors(X, n_neighbors?) */
FN(m_kneighbors) {
    MODEL;
    Data x = NO_DATA;
    LubeValue err;
    double k = argc > 2 ? lube->number(argv[2]) : m->p[P_N_NEIGHBORS];
    if (argc > 2 && (lube->kind(argv[2]) != LUBE_NUMBER || k < 1 || k != floor(k)))
        return lube->error(vm, "kneighbors(): n_neighbors must be a whole Number, 1 or more");
    if (!x_for(vm, "kneighbors", m, argv[1], &x, &err)) return err;
    LubeValue r = kneighbors(vm, m, &x, (int64_t)k);
    data_free(&x);
    return r;
}

FN(m_get_depth) {
    (void)argc;
    MODEL;
    return m->fitted ? lube->number_value(m->trees[0].depth) : not_fitted(vm, FNAME, m);
}

FN(m_get_n_leaves) {
    (void)argc;
    MODEL;
    if (!m->fitted) return not_fitted(vm, FNAME, m);
    int leaves = 0;
    for (int i = 0; i < m->trees[0].count; i++) leaves += m->trees[0].nodes[i].feature < 0;
    return lube->number_value(leaves);
}

/* ---------- LabelEncoder: labels (Numbers or text) to 0, 1, 2... ---------- */

static int is_text_list(LubeValue v) {
    return lube->kind(v) == LUBE_LIST && lube->list_len(v) && lube->kind(lube->list_get(v, 0)) == LUBE_STR;
}

static int cmp_text(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Where text s is among m's classes, or -1. */
static int64_t find_text(const Model *m, const char *s) {
    int64_t lo = 0, hi = m->n_text - 1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2, c = strcmp(m->text[mid], s);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid - 1;
        else return mid;
    }
    return -1;
}

static LubeValue label_fit(LubeVM *vm, const char *fname, Model *m, LubeValue y) {
    model_reset(m);
    if (is_text_list(y)) {
        size_t n = lube->list_len(y);
        char **all = (char **)alloc(sizeof(char *) * n);
        for (size_t i = 0; i < n; i++) {
            const char *s = lube->str(lube->list_get(y, i), NULL);
            if (!s) {
                free(all);
                return lube->error(vm, "%s(): y mixes text with %s", fname, lube->a_type(lube->list_get(y, i)));
            }
            all[i] = (char *)s;
        }
        qsort(all, n, sizeof(char *), cmp_text);
        m->text = (char **)alloc(sizeof(char *) * n);
        for (size_t i = 0; i < n; i++)
            if (i == 0 || strcmp(all[i], all[i - 1])) {
                size_t len = strlen(all[i]) + 1;
                m->text[m->n_text] = (char *)alloc(len);
                memcpy(m->text[m->n_text++], all[i], len);
            }
        free(all);
    } else {
        Data d = NO_DATA;
        LubeValue err;
        if (!data_in(vm, fname, "y", y, 0, W_VECTOR, &d, &err)) return err;
        int64_t k;
        free(classes_fit(vm, "LabelEncoder", m, &d, 1, &k, &err));
        data_free(&d);
    }
    m->fitted = 1;
    return lube->nothing();
}

static LubeValue label_transform(LubeVM *vm, const char *fname, const Model *m, LubeValue y) {
    if (!m->fitted) return not_fitted(vm, fname, m);
    int64_t *out;
    if (m->text) {
        if (lube->kind(y) != LUBE_LIST) return lube->error(vm, "%s(): this LabelEncoder knows text labels: y must be a list of them", fname);
        int64_t n = (int64_t)lube->list_len(y);
        LubeValue r = new_i64(vm, n, -1, &out);
        for (int64_t i = 0; i < n; i++) {
            const char *s = lube->str(lube->list_get(y, (size_t)i), NULL);
            out[i] = s ? find_text(m, s) : -1;
            if (out[i] < 0) {
                char got[128];
                lube->release(r);
                return lube->error(vm, "%s(): y has a label LabelEncoder did not see in fit: %s", fname,
                                   value_text(lube->list_get(y, (size_t)i), got, sizeof got));
            }
        }
        return r;
    }
    Data d = NO_DATA;
    LubeValue err;
    if (!data_in(vm, fname, "y", y, 0, W_VECTOR, &d, &err)) return err;
    LubeValue r = new_i64(vm, d.n, -1, &out);
    for (int64_t i = 0; i < d.n; i++)
        if ((out[i] = find_value(m->s[S_CLASSES].v, m->s[S_CLASSES].shape[0], d.v[i])) < 0) {
            char num[64];
            lube->format_number(d.v[i], num, sizeof num);
            lube->release(r);
            data_free(&d);
            return lube->error(vm, "%s(): y has a label LabelEncoder did not see in fit: %s", fname, num);
        }
    data_free(&d);
    return r;
}

FN(m_label_fit) {
    (void)argc;
    LubeValue r = label_fit(vm, "fit", model_of(argv[0]), argv[1]);
    return lube->failed(r) ? r : lube->retain(argv[0]);
}

FN(m_label_transform) {
    (void)argc;
    return label_transform(vm, "transform", model_of(argv[0]), argv[1]);
}

FN(m_label_fit_transform) {
    (void)argc;
    MODEL;
    LubeValue r = label_fit(vm, "fit_transform", m, argv[1]);
    return lube->failed(r) ? r : label_transform(vm, "fit_transform", m, argv[1]);
}

FN(m_label_inverse) {
    (void)argc;
    MODEL;
    Data d = NO_DATA;
    LubeValue err;
    if (!m->fitted) return not_fitted(vm, "inverse_transform", m);
    if (!data_in(vm, "inverse_transform", "y", argv[1], 0, W_VECTOR, &d, &err)) return err;
    int64_t k = m->text ? m->n_text : m->s[S_CLASSES].shape[0];
    for (int64_t i = 0; i < d.n; i++)
        if (d.v[i] != floor(d.v[i]) || d.v[i] < 0 || d.v[i] >= (double)k) {
            char num[64];
            lube->format_number(d.v[i], num, sizeof num);
            data_free(&d);
            return lube->error(vm, "inverse_transform(): %s is not a label's number (0 to %.0f)", num, (double)(k - 1));
        }
    LubeValue r;
    if (m->text) {
        r = lube->new_list(vm);
        for (int64_t i = 0; i < d.n; i++) {
            const char *s = m->text[(int64_t)d.v[i]];
            lube->list_push(r, lube->new_str(vm, s, strlen(s)));
        }
    } else {
        int *codes = (int *)alloc(sizeof(int) * (size_t)d.n);
        for (int64_t i = 0; i < d.n; i++) codes[i] = (int)d.v[i];
        r = labels_out(vm, m, codes, d.n);
        free(codes);
    }
    data_free(&d);
    return r;
}

/* ======================================================================
 * Properties: what fit found
 * ==================================================================== */

static LubeValue slot_property(LubeVM *vm, LubeValue self, int slot) {
    Model *m = model_of(self);
    if (!m->fitted) return lube->error(vm, "%s: this %s is not fitted yet: call fit() first", SLOT_NAMES[slot], KIND_NAMES[m->kind]);
    if (slot == S_CLASSES && m->text) {
        LubeValue list = lube->new_list(vm);
        for (int64_t i = 0; i < m->n_text; i++) lube->list_push(list, lube->new_str(vm, m->text[i], strlen(m->text[i])));
        return list;
    }
    if (slot == S_CLASSES) {  /* (in the labels' dtype) */
        const Slot *s = &m->s[S_CLASSES];
        return values_out(vm, s->v, 1, s->shape, m->label_dtype == ARR_I64);
    }
    return slot_out(vm, &m->s[slot]);
}

#define PROP(i) FN(p_##i) { (void)argc; return slot_property(vm, argv[0], i); }
PROP(0) PROP(1) PROP(2) PROP(3) PROP(4) PROP(5) PROP(6) PROP(7) PROP(8) PROP(9) PROP(10) PROP(11) PROP(12) PROP(13)
PROP(14) PROP(15) PROP(16) PROP(17) PROP(18) PROP(19) PROP(20) PROP(21) PROP(22) PROP(23) PROP(24) PROP(25) PROP(26)
PROP(27) PROP(28) PROP(29)
static const LubeFn PROPS[NSHOWN] = {p_0, p_1, p_2, p_3, p_4, p_5, p_6, p_7, p_8, p_9, p_10, p_11, p_12, p_13, p_14,
                                     p_15, p_16, p_17, p_18, p_19, p_20, p_21, p_22, p_23, p_24, p_25, p_26, p_27,
                                     p_28, p_29};

FN(p_tree) {
    (void)argc;
    MODEL;
    return m->fitted ? tree_object(vm, &m->trees[0]) : lube->error(vm, "tree_: this %s is not fitted yet: call fit() first", KIND_NAMES[m->kind]);
}

FN(p_categories) {
    (void)argc;
    MODEL;
    return m->fitted ? onehot_categories(vm, m) : lube->error(vm, "categories_: this OneHotEncoder is not fitted yet: call fit() first");
}

/* ======================================================================
 * The types
 * ==================================================================== */

#define CTOR(k) FN(c_##k) { return construct(vm, k, argc, argv); }
CTOR(0) CTOR(1) CTOR(2) CTOR(3) CTOR(4) CTOR(5) CTOR(6) CTOR(7) CTOR(8) CTOR(9) CTOR(10) CTOR(11) CTOR(12) CTOR(13)
CTOR(14) CTOR(15) CTOR(16) CTOR(17) CTOR(18) CTOR(19) CTOR(20) CTOR(21) CTOR(22)
static const LubeFn CTORS[NKINDS] = {c_0, c_1, c_2, c_3, c_4, c_5, c_6, c_7, c_8, c_9, c_10, c_11, c_12, c_13, c_14,
                                     c_15, c_16, c_17, c_18, c_19, c_20, c_21, c_22};

FN(m_split);
FN(m_get_n_splits);

static void declare_models(LubePlugin *plugin) {
    for (int k = 0; k < NKINDS; k++) {
        const Kind *kd = &KINDS[k];
        LubeType *t = TYPES[k] = lube->type(plugin, KIND_NAMES[k], sizeof(Model), model_release, model_describe);
        lube->constructor(t, CTORS[k], 0, 2);
        lube->method(t, "get_params", m_get_params, 1, 1);
        lube->method(t, "set_params", m_set_params, 2, 2);
        for (const signed char *p = kd->props; *p >= 0; p++) lube->property(t, SLOT_NAMES[*p], PROPS[*p]);
        if (k == K_LABEL_ENCODER) {
            lube->method(t, "fit", m_label_fit, 2, 2);
            lube->method(t, "transform", m_label_transform, 2, 2);
            lube->method(t, "fit_transform", m_label_fit_transform, 2, 2);
            lube->method(t, "inverse_transform", m_label_inverse, 2, 2);
            continue;
        }
        if (kd->role == R_SPLITTER) {
            lube->method(t, "split", m_split, 2, 3);
            lube->method(t, "get_n_splits", m_get_n_splits, 1, 3);
            continue;
        }
        lube->method(t, "fit", m_fit, 2, 3);
        if (kd->predict) lube->method(t, "predict", m_predict, 2, 2);
        if (kd->proba) lube->method(t, "predict_proba", m_predict_proba, 2, 2);
        if (kd->log_proba) lube->method(t, "predict_log_proba", m_predict_log_proba, 2, 2);
        if (kd->decision) lube->method(t, "decision_function", m_decision_function, 2, 2);
        if (kd->transform) {
            lube->method(t, "transform", m_transform, 2, 2);
            lube->method(t, "fit_transform", m_fit_transform, 2, 3);
        }
        if (kd->inverse) lube->method(t, "inverse_transform", m_inverse_transform, 2, 2);
        if (kd->role == R_CLASSIFIER || kd->role == R_REGRESSOR || k == K_KMEANS) lube->method(t, "score", m_score, 2, 3);
        if (kd->role == R_CLUSTER) lube->method(t, "fit_predict", m_fit_predict, 2, 3);
    }
    lube->method(TYPES[K_KNN_CLASSIFIER], "kneighbors", m_kneighbors, 2, 3);
    lube->method(TYPES[K_KNN_REGRESSOR], "kneighbors", m_kneighbors, 2, 3);
    for (int k = K_TREE_CLASSIFIER; k <= K_TREE_REGRESSOR; k++) {
        lube->method(TYPES[k], "get_depth", m_get_depth, 1, 1);
        lube->method(TYPES[k], "get_n_leaves", m_get_n_leaves, 1, 1);
        lube->method(TYPES[k], "apply", m_apply, 2, 2);
        lube->property(TYPES[k], "tree_", p_tree);
    }
    lube->property(TYPES[K_ONEHOT_ENCODER], "categories_", p_categories);
}
