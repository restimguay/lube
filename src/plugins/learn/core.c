/*
 * learn/core.c - what every model uses: its options, its data, Arrays in
 * and out, labels, random numbers, and the numerical parts (Cholesky,
 * symmetric eigenvectors, L-BFGS). (Part of learn.c.)
 */

/* ======================================================================
 * Options: each model takes some of these, with defaults of its own
 * ==================================================================== */

enum {
    P_ALPHA, P_L1_RATIO, P_C, P_FIT_INTERCEPT, P_MAX_ITER, P_TOL, P_N_NEIGHBORS, P_WEIGHTS, P_P, P_CRITERION,
    P_CRITERION_R, P_MAX_DEPTH, P_MIN_SAMPLES_SPLIT, P_MIN_SAMPLES_LEAF, P_MAX_FEATURES, P_N_ESTIMATORS, P_BOOTSTRAP,
    P_RANDOM_STATE, P_VAR_SMOOTHING, P_N_CLUSTERS, P_INIT, P_N_INIT, P_EPS, P_MIN_SAMPLES, P_N_COMPONENTS, P_WHITEN,
    P_WITH_MEAN, P_WITH_STD, P_FEATURE_RANGE, P_RANGE_HI, P_CLIP, P_HANDLE_UNKNOWN, P_DROP, P_DEGREE,
    P_INTERACTION_ONLY, P_INCLUDE_BIAS, P_N_SPLITS, P_SHUFFLE, NPARAMS
};

enum {
    T_NUM,         /* a Number from lo to hi */
    T_POS,         /* a Number more than 0 */
    T_WHOLE,       /* a whole Number from lo to hi */
    T_BOOL,
    T_CHOICE,      /* one of the texts in choices (kept as its index) */
    T_SEED,        /* random_state: a whole Number from 0 to 2^32 - 1 (-1 when not given) */
    T_DEPTH,       /* a whole Number, 1 or more (-1: none) */
    T_FEATURES,    /* max_features: "sqrt", "log2", a count, or a share below 1 (-1: all, -2 sqrt, -3 log2) */
    T_COMPONENTS,  /* n_components: a count, or a share of the variance below 1 (-1: all) */
    T_AUTO,        /* a whole Number from lo, or "auto" (-1) */
    T_RANGE        /* feature_range: [low, high] (low here, high in the next entry) */
};

static const struct {
    const char *name;
    int type;
    double lo, hi;
    const char *choices;  /* T_CHOICE's, "|" between them */
} PARAMS[NPARAMS] = {
    {"alpha", T_NUM, 0, INFINITY, NULL}, {"l1_ratio", T_NUM, 0, 1, NULL}, {"C", T_POS, 0, 0, NULL},
    {"fit_intercept", T_BOOL, 0, 0, NULL}, {"max_iter", T_WHOLE, 1, 1e9, NULL}, {"tol", T_NUM, 0, INFINITY, NULL},
    {"n_neighbors", T_WHOLE, 1, 1e9, NULL}, {"weights", T_CHOICE, 0, 0, "uniform|distance"},
    {"p", T_WHOLE, 1, 2, NULL}, {"criterion", T_CHOICE, 0, 0, "gini|entropy|log_loss"},
    {"criterion", T_CHOICE, 0, 0, "squared_error|friedman_mse"}, {"max_depth", T_DEPTH, 1, 1e9, NULL},
    {"min_samples_split", T_WHOLE, 2, 1e15, NULL}, {"min_samples_leaf", T_WHOLE, 1, 1e15, NULL},
    {"max_features", T_FEATURES, 0, 0, "sqrt|log2|all"}, {"n_estimators", T_WHOLE, 1, 1e6, NULL},
    {"bootstrap", T_BOOL, 0, 0, NULL}, {"random_state", T_SEED, 0, 4294967295.0, NULL},
    {"var_smoothing", T_NUM, 0, INFINITY, NULL}, {"n_clusters", T_WHOLE, 1, 1e9, NULL},
    {"init", T_CHOICE, 0, 0, "k-means++|random"}, {"n_init", T_AUTO, 1, 1e6, NULL}, {"eps", T_POS, 0, 0, NULL},
    {"min_samples", T_WHOLE, 1, 1e15, NULL}, {"n_components", T_COMPONENTS, 0, 0, NULL},
    {"whiten", T_BOOL, 0, 0, NULL}, {"with_mean", T_BOOL, 0, 0, NULL}, {"with_std", T_BOOL, 0, 0, NULL},
    {"feature_range", T_RANGE, 0, 0, NULL}, {"", T_NUM, 0, 0, NULL}, {"clip", T_BOOL, 0, 0, NULL},
    {"handle_unknown", T_CHOICE, 0, 0, "error|ignore"}, {"drop", T_CHOICE, 0, 0, "none|first|if_binary"},
    {"degree", T_WHOLE, 0, 100, NULL}, {"interaction_only", T_BOOL, 0, 0, NULL}, {"include_bias", T_BOOL, 0, 0, NULL},
    {"n_splits", T_WHOLE, 2, 1e15, NULL}, {"shuffle", T_BOOL, 0, 0, NULL},
};

/* The i-th choice of a T_CHOICE option, into buf. */
static const char *choice_name(int param, int i, char *buf, size_t size) {
    const char *s = PARAMS[param].choices;
    for (; i > 0; i--) s = strchr(s, '|') + 1;
    size_t n = strcspn(s, "|");
    if (n >= size) n = size - 1;  /* LCOV_EXCL_LINE: (every choice is short) */
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

/* The index of text among param's choices, or -1. */
static int choice_index(int param, const char *text) {
    const char *s = PARAMS[param].choices;
    size_t len = strlen(text);
    for (int i = 0;; i++) {
        size_t n = strcspn(s, "|");
        if (n == len && !strncmp(s, text, n)) return i;
        if (!s[n]) return -1;
        s += n + 1;
    }
}

/* ======================================================================
 * Models: one struct for all of them; each kind uses the parts it needs
 * ==================================================================== */

enum {  /* the kinds of model (the order of KINDS, in models.c) */
    K_LINEAR_REGRESSION, K_RIDGE, K_LASSO, K_ELASTIC_NET, K_LOGISTIC_REGRESSION, K_LINEAR_SVC, K_KNN_CLASSIFIER,
    K_KNN_REGRESSOR, K_TREE_CLASSIFIER, K_TREE_REGRESSOR, K_FOREST_CLASSIFIER, K_FOREST_REGRESSOR, K_GAUSSIAN_NB,
    K_KMEANS, K_DBSCAN, K_PCA, K_STANDARD_SCALER, K_MINMAX_SCALER, K_LABEL_ENCODER, K_ONEHOT_ENCODER,
    K_POLYNOMIAL_FEATURES, K_KFOLD, K_STRATIFIED_KFOLD, NKINDS
};

static const char *const KIND_NAMES[NKINDS] = {
    "LinearRegression", "Ridge", "Lasso", "ElasticNet", "LogisticRegression", "LinearSVC", "KNeighborsClassifier",
    "KNeighborsRegressor", "DecisionTreeClassifier", "DecisionTreeRegressor", "RandomForestClassifier",
    "RandomForestRegressor", "GaussianNB", "KMeans", "DBSCAN", "PCA", "StandardScaler", "MinMaxScaler", "LabelEncoder",
    "OneHotEncoder", "PolynomialFeatures", "KFold", "StratifiedKFold",
};

/* What fit found, shown as properties (S_X and after are kept but not shown). */
enum {
    S_COEF, S_INTERCEPT, S_CLASSES, S_N_ITER, S_CENTERS, S_LABELS, S_INERTIA, S_COMPONENTS, S_EVAR, S_EVAR_RATIO,
    S_SINGULAR, S_MEAN, S_NOISE, S_N_COMPONENTS, S_SCALE, S_VAR, S_N_SEEN, S_MIN, S_DATA_MIN, S_DATA_MAX,
    S_DATA_RANGE, S_THETA, S_PRIOR, S_COUNT, S_EPSILON, S_IMPORTANCES, S_CORE, S_POWERS, S_N_OUT, S_N_FEATURES,
    NSHOWN, S_X = NSHOWN, S_Y, S_CATS, S_CAT_START, NSLOTS
};

static const char *const SLOT_NAMES[NSHOWN] = {
    "coef_", "intercept_", "classes_", "n_iter_", "cluster_centers_", "labels_", "inertia_", "components_",
    "explained_variance_", "explained_variance_ratio_", "singular_values_", "mean_", "noise_variance_",
    "n_components_", "scale_", "var_", "n_samples_seen_", "min_", "data_min_", "data_max_", "data_range_", "theta_",
    "class_prior_", "class_count_", "epsilon_", "feature_importances_", "core_sample_indices_", "powers_",
    "n_output_features_", "n_features_in_",
};

typedef struct {
    double *v;         /* NULL until fit sets it */
    int nd;            /* 0: a Number (v[0]), else 1 or 2 dimensions */
    int ints;          /* shown as an int64 Array */
    int64_t shape[2];
} Slot;

/* A decision tree (tree.c builds them). */
typedef struct {
    int feature;       /* -1 for a leaf */
    int left, right;   /* the children's nodes */
    double threshold;  /* samples with x[feature] <= threshold go left */
    double impurity;
    int64_t n;         /* samples in the node */
    double wn;         /* their weight */
} Node;

typedef struct {
    Node *nodes;
    double *value;       /* k per node: the share of each class, or the mean */
    double *importance;  /* each feature's (summed) decrease of impurity */
    int count, cap, k, depth;
} Tree;

static void tree_free(Tree *t) {
    free(t->nodes);
    free(t->value);
    free(t->importance);
}

typedef struct {
    int kind;
    int fitted;
    double p[NPARAMS];
    unsigned char given[NPARAMS];  /* (options given to the constructor: they print) */
    int64_t n_features;            /* X's columns, at fit */
    int label_dtype;               /* the dtype predictions come in: y's at fit */
    Slot s[NSLOTS];
    Tree *trees;                   /* a tree, or a forest's */
    int n_trees;
    char **text;                   /* LabelEncoder's classes when they are text */
    int64_t n_text;
} Model;

/* Forgets what fit found. */
static void model_reset(Model *m) {
    for (int i = 0; i < NSLOTS; i++) {
        free(m->s[i].v);
        m->s[i].v = NULL;
    }
    for (int i = 0; i < m->n_trees; i++) tree_free(&m->trees[i]);
    free(m->trees);
    m->trees = NULL;
    m->n_trees = 0;
    for (int64_t i = 0; i < m->n_text; i++) free(m->text[i]);
    free(m->text);
    m->text = NULL;
    m->n_text = 0;
    m->fitted = 0;
}

static void *alloc(size_t bytes) {
    void *p = calloc(1, bytes ? bytes : 1);
    if (!p) abort();  /* LCOV_EXCL_LINE: out of memory */
    return p;
}

/* Slot i, made (zeros) with a shape: nd 0 is a Number; ints says it shows as int64. */
static double *slot_make(Model *m, int i, int nd, int64_t rows, int64_t cols, int ints) {
    Slot *s = &m->s[i];
    free(s->v);
    s->nd = nd;
    s->ints = ints;
    s->shape[0] = nd > 0 ? rows : 1;
    s->shape[1] = nd > 1 ? cols : 1;
    s->v = (double *)alloc(sizeof(double) * (size_t)(s->shape[0] * s->shape[1]));
    return s->v;
}

static void slot_number(Model *m, int i, double x, int ints) { *slot_make(m, i, 0, 1, 1, ints) = x; }

/* ======================================================================
 * Arrays in: X (a row per sample) and y
 * ==================================================================== */

typedef struct {
    LubeValue keep;  /* the float64 C-contiguous Array: let go of it with data_free */
    const double *v;
    int64_t n, d;    /* rows, and columns (1 for a vector) */
    int nd;
    int dtype;       /* what it came as (for labels) */
} Data;

enum { W_MATRIX, W_VECTOR, W_EITHER };  /* what data_in wants */
#define NO_DATA {{{0, 0}}, NULL, 0, 0, 0, 0}  /* (data_in sets it: for compilers that cannot tell) */

static void data_free(Data *x) {
    lube->release(x->keep);
    x->keep = lube->nothing();
}

/* fname's argument argi (what: "X", "y"...) as float64 numbers: 0 with *err after an error. */
static int data_in(LubeVM *vm, const char *fname, const char *what, LubeValue v, int argi, int want, Data *out,
                   LubeValue *err) {
    out->keep = lube->nothing();
    Array *src = arr->get(v);
    int k = lube->kind(v);
    if (!src && k != LUBE_LIST) {
        *err = lube->error(vm, "%s() expects an Array (or a list) as argument %d (%s), got %s", fname, argi + 1, what,
                           lube->a_type(v));
        return 0;
    }
    if (k == LUBE_LIST && lube->list_len(v) && lube->kind(lube->list_get(v, 0)) == LUBE_STR) {
        *err = lube->error(vm, "%s(): %s must be Numbers, not text (LabelEncoder turns text labels into Numbers)",
                           fname, what);
        return 0;
    }
    out->dtype = src ? src->dtype : ARR_F64;
    LubeValue a = arr->from(vm, v, ARR_F64);
    if (lube->failed(a)) {
        *err = a;
        return 0;
    }
    out->keep = arr->contiguous(vm, a, ARR_F64);
    lube->release(a);
    Array *x = arr->get(out->keep);
    out->v = (const double *)x->data;
    out->nd = x->ndim;
    if (x->ndim == 2 && want == W_VECTOR && x->shape[1] == 1) out->nd = 1;  /* (a column: as a vector) */
    if ((want == W_MATRIX && x->ndim != 2) || (want == W_VECTOR && out->nd != 1) || (want == W_EITHER && (x->ndim < 1 || x->ndim > 2))) {
        int nd = x->ndim;
        data_free(out);
        if (want == W_MATRIX && nd == 1)
            *err = lube->error(vm, "%s(): %s must be 2-dimensional (a row per sample), not 1-dimensional: "
                                   "x.reshape(-1, 1) makes a column of it", fname, what);
        else
            *err = lube->error(vm, "%s(): %s must be %s, not %d-dimensional", fname, what,
                               want == W_MATRIX ? "2-dimensional (a row per sample)"
                               : want == W_VECTOR ? "1-dimensional" : "1- or 2-dimensional", nd);
        return 0;
    }
    out->n = x->ndim ? x->shape[0] : 1;
    out->d = x->ndim == 2 ? x->shape[1] : 1;
    if (out->n == 0 || (out->nd == 2 && out->d == 0)) {
        data_free(out);
        *err = lube->error(vm, "%s(): %s is empty", fname, what);
        return 0;
    }
    for (int64_t i = 0; i < x->size; i++)
        if (!isfinite(out->v[i])) {
            data_free(out);
            *err = lube->error(vm, "%s(): %s has NaN or infinity in it", fname, what);
            return 0;
        }
    return 1;
}

/* X and y for fit: as many samples in each. */
static int xy_in(LubeVM *vm, const char *fname, LubeValue xv, LubeValue yv, int ywant, Data *x, Data *y,
                 LubeValue *err) {
    if (!data_in(vm, fname, "X", xv, 0, W_MATRIX, x, err)) return 0;
    if (!data_in(vm, fname, "y", yv, 1, ywant, y, err)) {
        data_free(x);
        return 0;
    }
    if (x->n != y->n) {
        *err = lube->error(vm, "%s(): X has %.0f samples but y has %.0f", fname, (double)x->n, (double)y->n);
        data_free(x);
        data_free(y);
        return 0;
    }
    return 1;
}

/* ======================================================================
 * Arrays out
 * ==================================================================== */

/* A new float64 Array, 1-dimensional (cols < 0) or 2: its memory in *out. */
static LubeValue new_f64(LubeVM *vm, int64_t rows, int64_t cols, double **out) {
    int64_t shape[2] = {rows, cols};
    void *data;
    LubeValue v = arr->make(vm, ARR_F64, cols < 0 ? 1 : 2, shape, &data);
    *out = (double *)data;
    return v;
}

static LubeValue new_i64(LubeVM *vm, int64_t rows, int64_t cols, int64_t **out) {
    int64_t shape[2] = {rows, cols};
    void *data;
    LubeValue v = arr->make(vm, ARR_I64, cols < 0 ? 1 : 2, shape, &data);
    *out = (int64_t *)data;
    return v;
}

/* An Array (or Number) of n doubles, as dtype (ARR_F64 or ARR_I64). */
static LubeValue values_out(LubeVM *vm, const double *v, int nd, const int64_t *shape, int ints) {
    if (nd == 0) return lube->number_value(v[0]);
    void *data;
    LubeValue r = arr->make(vm, ints ? ARR_I64 : ARR_F64, nd, shape, &data);
    int64_t n = shape[0] * (nd > 1 ? shape[1] : 1);
    if (ints)
        for (int64_t i = 0; i < n; i++) ((int64_t *)data)[i] = (int64_t)v[i];
    else if (n)
        memcpy(data, v, sizeof(double) * (size_t)n);
    return r;
}

static LubeValue slot_out(LubeVM *vm, const Slot *s) { return values_out(vm, s->v, s->nd, s->shape, s->ints); }

/* ======================================================================
 * Labels: the classes are y's distinct values, sorted
 * ==================================================================== */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* y's distinct values, sorted (a new array; *k of them). */
static double *distinct(const double *y, int64_t n, int64_t *k) {
    double *u = (double *)alloc(sizeof(double) * (size_t)n);
    memcpy(u, y, sizeof(double) * (size_t)n);
    qsort(u, (size_t)n, sizeof(double), cmp_double);
    int64_t m = 0;
    for (int64_t i = 0; i < n; i++)
        if (i == 0 || u[i] != u[m - 1]) u[m++] = u[i];
    *k = m;
    return u;
}

/* Where x is among k sorted values, or -1. */
static int64_t find_value(const double *sorted, int64_t k, double x) {
    int64_t lo = 0, hi = k - 1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2;
        if (sorted[mid] < x) lo = mid + 1;
        else if (sorted[mid] > x) hi = mid - 1;
        else return mid;
    }
    return -1;
}

/* For a classifier's fit: its classes_ from y, and each sample's class (a new array). 0 with *err if there is
 * only one class (unless one is allowed). */
static int *classes_fit(LubeVM *vm, const char *name, Model *m, const Data *y, int one_ok, int64_t *k, LubeValue *err) {
    double *u = distinct(y->v, y->n, k);
    if (*k < 2 && !one_ok) {
        free(u);
        *err = lube->error(vm, "fit(): %s needs at least 2 classes in y, got 1", name);
        return NULL;
    }
    double *c = slot_make(m, S_CLASSES, 1, *k, 1, 0);
    memcpy(c, u, sizeof(double) * (size_t)*k);
    free(u);
    int *codes = (int *)alloc(sizeof(int) * (size_t)y->n);
    for (int64_t i = 0; i < y->n; i++) codes[i] = (int)find_value(c, *k, y->v[i]);
    m->label_dtype = y->dtype == ARR_F64 || y->dtype == ARR_F32 ? ARR_F64 : ARR_I64;
    return codes;
}

/* Predictions: class codes[i] of each sample, as labels. */
static LubeValue labels_out(LubeVM *vm, const Model *m, const int *codes, int64_t n) {
    const double *c = m->s[S_CLASSES].v;
    void *data;
    LubeValue r = arr->make(vm, m->label_dtype, 1, &n, &data);
    for (int64_t i = 0; i < n; i++) {
        if (m->label_dtype == ARR_I64) ((int64_t *)data)[i] = (int64_t)c[codes[i]];
        else ((double *)data)[i] = c[codes[i]];
    }
    return r;
}

/* Predictions from each sample's k scores (rows of p): the class of the highest. */
static LubeValue argmax_labels(LubeVM *vm, const Model *m, const double *p, int64_t n, int64_t k) {
    int *codes = (int *)alloc(sizeof(int) * (size_t)n);
    for (int64_t i = 0; i < n; i++) {
        int best = 0;
        for (int64_t c = 1; c < k; c++)
            if (p[i * k + c] > p[i * k + best]) best = (int)c;
        codes[i] = best;
    }
    LubeValue r = labels_out(vm, m, codes, n);
    free(codes);
    return r;
}

/* ======================================================================
 * Random numbers: each call that draws has a generator of its own, numpy's
 * (the Mersenne Twister of its RandomState, as array's is): seeded with
 * random_state, it gives the numbers RandomState(random_state) gives, so
 * shuffles and splits are sklearn's; without one, it is seeded from array's
 * generator (np.seed governs it). Nothing is shared: isolates can't upset
 * each other's draws. Trees on threads each have a small one (splitmix64).
 * ==================================================================== */

typedef struct {
    uint32_t key[624];
    int pos, has_gauss;
    double gauss;
} Mt;

static void mt_seed(Mt *r, uint32_t seed) {
    for (int i = 0; i < 624; i++) {
        r->key[i] = seed;
        seed = 1812433253u * (seed ^ (seed >> 30)) + (uint32_t)i + 1;
    }
    r->pos = 624;
    r->has_gauss = 0;
}

static uint32_t mt_next32(Mt *r) {
    if (r->pos == 624) {
        for (int i = 0; i < 624; i++) {
            uint32_t y = (r->key[i] & 0x80000000u) | (r->key[(i + 1) % 624] & 0x7fffffffu);
            r->key[i] = r->key[(i + 397) % 624] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfu : 0);
        }
        r->pos = 0;
    }
    uint32_t y = r->key[r->pos++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

static double mt_double(Mt *r) {  /* [0, 1), 53 random bits */
    uint32_t a = mt_next32(r) >> 5, b = mt_next32(r) >> 6;
    return (a * 67108864.0 + b) / 9007199254740992.0;
}

static double mt_gauss(Mt *r) {  /* numpy's legacy polar method (the second value kept for next time) */
    if (r->has_gauss) {
        r->has_gauss = 0;
        return r->gauss;
    }
    double x1, x2, r2;
    do {
        x1 = 2.0 * mt_double(r) - 1.0;
        x2 = 2.0 * mt_double(r) - 1.0;
        r2 = x1 * x1 + x2 * x2;
    } while (r2 >= 1.0 || r2 == 0.0);
    double f = sqrt(-2.0 * log(r2) / r2);
    r->gauss = f * x1;
    r->has_gauss = 1;
    return f * x2;
}

/* A whole number in 0..max, each as likely (numpy's masked rejection: what its shuffle draws). */
static uint32_t mt_below(Mt *r, uint32_t max) {
    uint32_t mask = max, v;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    while ((v = mt_next32(r) & mask) > max) {}
    return v;
}

/* n numbers into out: uniform in [a, b), or normal with mean a and deviation b (as numpy draws them). */
static void mt_fill(Mt *r, double *out, int64_t n, int normal, double a, double b) {
    for (int64_t i = 0; i < n; i++) out[i] = normal ? a + b * mt_gauss(r) : a + (b - a) * mt_double(r);
}

/* idx shuffled as numpy's shuffle does it. */
static void mt_shuffle(Mt *r, int64_t *idx, int64_t n) {
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = mt_below(r, (uint32_t)i), t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
    }
}

/* The generator of a call (to free): seeded with random_state, or (-1) from array's generator. */
static Mt *mt_for(double random_state) {
    Mt *r = (Mt *)alloc(sizeof(Mt));
    double u = 0;
    if (random_state < 0) arr->fill_random(&u, ARR_F64, 1, 0, 0.0, 4294967296.0);
    mt_seed(r, random_state < 0 ? (uint32_t)u : (uint32_t)random_state);
    return r;
}

typedef struct { uint64_t s; } Rng;  /* splitmix64 */

static uint64_t rng_next(Rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int64_t rng_below(Rng *r, int64_t n) { return (int64_t)((double)(rng_next(r) >> 11) / 9007199254740992.0 * (double)n); }

/* ======================================================================
 * Numbers
 * ==================================================================== */

/* a^T b for float64 Arrays a (n x p) and b (n x q, or n): a new p x q (or p) array, through array's matmul. */
static double *tmatmul(LubeVM *vm, LubeValue a, LubeValue b) {
    LubeValue at = arr->transpose(vm, a, NULL);
    LubeValue r = arr->matmul(vm, at, b);
    Array *ra = arr->get(r);
    double *out = (double *)alloc(sizeof(double) * (size_t)ra->size);
    memcpy(out, ra->data, sizeof(double) * (size_t)ra->size);
    lube->release(at);
    lube->release(r);
    return out;
}

/* X minus its column means (a new float64 Array, in *keep), the means in mean (if not NULL). */
static double *centered(LubeVM *vm, const Data *x, double *mean, LubeValue *keep) {
    double *c, *mu = (double *)alloc(sizeof(double) * (size_t)x->d);
    *keep = new_f64(vm, x->n, x->d, &c);
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t j = 0; j < x->d; j++) mu[j] += x->v[i * x->d + j];
    for (int64_t j = 0; j < x->d; j++) mu[j] /= (double)x->n;
    for (int64_t i = 0; i < x->n; i++)
        for (int64_t j = 0; j < x->d; j++) c[i * x->d + j] = x->v[i * x->d + j] - mu[j];
    if (mean) memcpy(mean, mu, sizeof(double) * (size_t)x->d);
    free(mu);
    return c;
}

/* a (n x n, symmetric positive definite) = L L^T, L in a's lower half: 0 if it is not positive definite. */
static int cholesky(double *a, int64_t n) {
    for (int64_t j = 0; j < n; j++) {
        double s = a[j * n + j];
        for (int64_t k = 0; k < j; k++) s -= a[j * n + k] * a[j * n + k];
        if (!(s > 0)) return 0;
        double d = sqrt(s);
        a[j * n + j] = d;
        for (int64_t i = j + 1; i < n; i++) {
            double t = a[i * n + j];
            for (int64_t k = 0; k < j; k++) t -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = t / d;
        }
    }
    return 1;
}

/* Solves L L^T x = b in place, for nrhs right-hand sides (b: n x nrhs). */
static void cholesky_solve(const double *l, int64_t n, double *b, int64_t nrhs) {
    for (int64_t r = 0; r < nrhs; r++) {
        for (int64_t i = 0; i < n; i++) {
            double s = b[i * nrhs + r];
            for (int64_t k = 0; k < i; k++) s -= l[i * n + k] * b[k * nrhs + r];
            b[i * nrhs + r] = s / l[i * n + i];
        }
        for (int64_t i = n - 1; i >= 0; i--) {
            double s = b[i * nrhs + r];
            for (int64_t k = i + 1; k < n; k++) s -= l[k * n + i] * b[k * nrhs + r];
            b[i * nrhs + r] = s / l[i * n + i];
        }
    }
}

/* The eigenvalues (ascending, into w) and eigenvectors (a's columns, replacing a) of the symmetric n x n a:
 * Householder to tridiagonal, then the implicit QL method (EISPACK's tred2 and tql2, as JAMA has them). */
static void eigh(double *a, int64_t n, double *w) {
    double *e = (double *)alloc(sizeof(double) * (size_t)n);
#define V(i, j) a[(i) * n + (j)]
    for (int64_t j = 0; j < n; j++) w[j] = V(n - 1, j);
    for (int64_t i = n - 1; i > 0; i--) {  /* tred2 */
        double scale = 0, h = 0;
        for (int64_t k = 0; k < i; k++) scale += fabs(w[k]);
        if (scale == 0) {
            e[i] = w[i - 1];
            for (int64_t j = 0; j < i; j++) {
                w[j] = V(i - 1, j);
                V(i, j) = 0;
                V(j, i) = 0;
            }
        } else {
            for (int64_t k = 0; k < i; k++) {
                w[k] /= scale;
                h += w[k] * w[k];
            }
            double f = w[i - 1], g = sqrt(h);
            if (f > 0) g = -g;
            e[i] = scale * g;
            h -= f * g;
            w[i - 1] = f - g;
            for (int64_t j = 0; j < i; j++) e[j] = 0;
            for (int64_t j = 0; j < i; j++) {
                f = w[j];
                V(j, i) = f;
                g = e[j] + V(j, j) * f;
                for (int64_t k = j + 1; k <= i - 1; k++) {
                    g += V(k, j) * w[k];
                    e[k] += V(k, j) * f;
                }
                e[j] = g;
            }
            f = 0;
            for (int64_t j = 0; j < i; j++) {
                e[j] /= h;
                f += e[j] * w[j];
            }
            double hh = f / (h + h);
            for (int64_t j = 0; j < i; j++) e[j] -= hh * w[j];
            for (int64_t j = 0; j < i; j++) {
                f = w[j];
                g = e[j];
                for (int64_t k = j; k <= i - 1; k++) V(k, j) -= (f * e[k] + g * w[k]);
                w[j] = V(i - 1, j);
                V(i, j) = 0;
            }
        }
        w[i] = h;
    }
    for (int64_t i = 0; i < n - 1; i++) {
        V(n - 1, i) = V(i, i);
        V(i, i) = 1;
        double h = w[i + 1];
        if (h != 0) {
            for (int64_t k = 0; k <= i; k++) w[k] = V(k, i + 1) / h;
            for (int64_t j = 0; j <= i; j++) {
                double g = 0;
                for (int64_t k = 0; k <= i; k++) g += V(k, i + 1) * V(k, j);
                for (int64_t k = 0; k <= i; k++) V(k, j) -= g * w[k];
            }
        }
        for (int64_t k = 0; k <= i; k++) V(k, i + 1) = 0;
    }
    for (int64_t j = 0; j < n; j++) {
        w[j] = V(n - 1, j);
        V(n - 1, j) = 0;
    }
    V(n - 1, n - 1) = 1;
    e[0] = 0;
    for (int64_t i = 1; i < n; i++) e[i - 1] = e[i];  /* tql2 */
    e[n - 1] = 0;
    double f = 0, tst1 = 0, eps = DBL_EPSILON;
    for (int64_t l = 0; l < n; l++) {
        tst1 = fmax(tst1, fabs(w[l]) + fabs(e[l]));
        int64_t m = l;
        while (m < n - 1 && fabs(e[m]) > eps * tst1) m++;
        if (m > l) {
            for (int iter = 0; iter < 100; iter++) {
                double g = w[l], p = (w[l + 1] - g) / (2 * e[l]), r = hypot(p, 1);
                if (p < 0) r = -r;
                w[l] = e[l] / (p + r);
                w[l + 1] = e[l] * (p + r);
                double dl1 = w[l + 1], h = g - w[l];
                for (int64_t i = l + 2; i < n; i++) w[i] -= h;
                f += h;
                p = w[m];
                double c = 1, c2 = 1, c3 = 1, el1 = e[l + 1], s = 0, s2 = 0;
                for (int64_t i = m - 1; i >= l; i--) {
                    c3 = c2;
                    c2 = c;
                    s2 = s;
                    g = c * e[i];
                    h = c * p;
                    r = hypot(p, e[i]);
                    e[i + 1] = s * r;
                    s = e[i] / r;
                    c = p / r;
                    p = c * w[i] - s * g;
                    w[i + 1] = h + s * (c * g + s * w[i]);
                    for (int64_t k = 0; k < n; k++) {
                        h = V(k, i + 1);
                        V(k, i + 1) = s * V(k, i) + c * h;
                        V(k, i) = c * V(k, i) - s * h;
                    }
                }
                p = -s * s2 * c3 * el1 * e[l] / dl1;
                e[l] = s * p;
                w[l] = c * p;
                if (!(fabs(e[l]) > eps * tst1)) break;
            }
        }
        w[l] += f;
        e[l] = 0;
    }
    for (int64_t i = 0; i < n - 1; i++) {  /* ascending */
        int64_t k = i;
        double p = w[i];
        for (int64_t j = i + 1; j < n; j++)
            if (w[j] < p) {
                k = j;
                p = w[j];
            }
        if (k != i) {
            w[k] = w[i];
            w[i] = p;
            for (int64_t j = 0; j < n; j++) {
                p = V(j, i);
                V(j, i) = V(j, k);
                V(j, k) = p;
            }
        }
    }
#undef V
    free(e);
}

/* Minimizes f (which also gives its gradient) from w, with L-BFGS (10 pairs kept) and a backtracking line search:
 * until the largest gradient entry is at most gtol, f stops going down, or max_iter. The iterations it took. */
typedef double (*Objective)(void *ctx, const double *w, double *grad);

static int lbfgs(Objective f, void *ctx, double *w, int64_t n, int max_iter, double gtol) {
    enum { M = 10 };
    double *g = (double *)alloc(sizeof(double) * (size_t)n * (4 + 2 * M)), *d = g + n, *wn = d + n, *gn = wn + n,
           *s = gn + n, *y = s + (size_t)n * M;
    double a[M], r[M];
    int stored = 0, head = 0, it = 0;
    double fx = f(ctx, w, g);
    for (; it < max_iter; it++) {
        double gmax = 0;
        for (int64_t i = 0; i < n; i++) gmax = fmax(gmax, fabs(g[i]));
        if (gmax <= gtol) break;
        /* d = -H g, by the two loops */
        for (int64_t i = 0; i < n; i++) d[i] = -g[i];
        for (int k = 0; k < stored; k++) {
            int j = (head - 1 - k + M) % M;
            double dot = 0;
            for (int64_t i = 0; i < n; i++) dot += s[j * n + i] * d[i];
            a[j] = r[j] * dot;
            for (int64_t i = 0; i < n; i++) d[i] -= a[j] * y[j * n + i];
        }
        if (stored) {
            int j = (head - 1 + M) % M;
            double sy = 0, yy = 0;
            for (int64_t i = 0; i < n; i++) {
                sy += s[j * n + i] * y[j * n + i];
                yy += y[j * n + i] * y[j * n + i];
            }
            for (int64_t i = 0; i < n; i++) d[i] *= sy / yy;
        }
        for (int k = stored - 1; k >= 0; k--) {
            int j = (head - 1 - k + M) % M;
            double dot = 0;
            for (int64_t i = 0; i < n; i++) dot += y[j * n + i] * d[i];
            double b = r[j] * dot;
            for (int64_t i = 0; i < n; i++) d[i] += (a[j] - b) * s[j * n + i];
        }
        double slope = 0;
        for (int64_t i = 0; i < n; i++) slope += g[i] * d[i];
        if (!(slope < 0)) {  /* LCOV_EXCL_START: (not a way down: start again from the gradient) */
            stored = 0;
            for (int64_t i = 0; i < n; i++) d[i] = -g[i];
            slope = 0;
            for (int64_t i = 0; i < n; i++) slope -= g[i] * g[i];
        }  /* LCOV_EXCL_STOP */
        double step = 1;
        if (!stored) {
            double norm = sqrt(-slope);
            step = norm > 1 ? 1 / norm : 1;
        }
        double fn = 0;
        int ok = 0;
        for (int ls = 0; ls < 60; ls++) {
            for (int64_t i = 0; i < n; i++) wn[i] = w[i] + step * d[i];
            fn = f(ctx, wn, gn);
            if (fn <= fx + 1e-4 * step * slope) {
                ok = 1;
                break;
            }
            step *= 0.5;
        }
        if (!ok) break;  /* LCOV_EXCL_LINE: (no step goes down: as far as it gets) */
        double sy = 0;
        for (int64_t i = 0; i < n; i++) {
            s[head * n + i] = wn[i] - w[i];
            y[head * n + i] = gn[i] - g[i];
            sy += s[head * n + i] * y[head * n + i];
        }
        if (sy > 1e-12) {
            r[head] = 1 / sy;
            head = (head + 1) % M;
            if (stored < M) stored++;
        }
        double drop = fx - fn;
        memcpy(w, wn, sizeof(double) * (size_t)n);
        memcpy(g, gn, sizeof(double) * (size_t)n);
        fx = fn;
        if (drop <= 64 * DBL_EPSILON * fmax(fmax(fabs(fx), fabs(fx + drop)), 1)) {
            it++;
            break;
        }
    }
    free(g);
    return it;
}

/* ======================================================================
 * Loops on several threads, in blocks of rows: each block's sums apart,
 * added up in order afterwards, so results never depend on the threads
 * ==================================================================== */

#define BLOCK 512

static int64_t blocks_of(int64_t n) { return (n + BLOCK - 1) / BLOCK; }

/* parallel over the blocks of n rows: on one thread when work (about the operations per row) is small. */
static void over_blocks(int64_t n, double work, void (*fn)(void *ctx, int64_t from, int64_t to), void *ctx) {
    int64_t nb = blocks_of(n);
    arr->parallel(nb, (double)n * work < 2e5 ? nb : 1, fn, ctx);
}
