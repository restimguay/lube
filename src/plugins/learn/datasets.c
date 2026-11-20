/*
 * learn/datasets.c - data to try models on: the iris flowers (Fisher's
 * 150, as sklearn has them), and made-up data: blobs, classes, regression
 * and two moons, drawn from array's generator (random_state seeds it).
 * (Part of learn.c.)
 */

/* The iris table, in tenths: sepal length and width, petal length and width (50 of each species, in order). */
static const unsigned char IRIS[600] = {
    51, 35, 14, 2, 49, 30, 14, 2, 47, 32, 13, 2, 46, 31, 15, 2, 50, 36, 14, 2, 54, 39, 17, 4, 46, 34, 14, 3, 50, 34,
    15, 2, 44, 29, 14, 2, 49, 31, 15, 1, 54, 37, 15, 2, 48, 34, 16, 2, 48, 30, 14, 1, 43, 30, 11, 1, 58, 40, 12, 2,
    57, 44, 15, 4, 54, 39, 13, 4, 51, 35, 14, 3, 57, 38, 17, 3, 51, 38, 15, 3, 54, 34, 17, 2, 51, 37, 15, 4, 46, 36,
    10, 2, 51, 33, 17, 5, 48, 34, 19, 2, 50, 30, 16, 2, 50, 34, 16, 4, 52, 35, 15, 2, 52, 34, 14, 2, 47, 32, 16, 2,
    48, 31, 16, 2, 54, 34, 15, 4, 52, 41, 15, 1, 55, 42, 14, 2, 49, 31, 15, 2, 50, 32, 12, 2, 55, 35, 13, 2, 49, 36,
    14, 1, 44, 30, 13, 2, 51, 34, 15, 2, 50, 35, 13, 3, 45, 23, 13, 3, 44, 32, 13, 2, 50, 35, 16, 6, 51, 38, 19, 4,
    48, 30, 14, 3, 51, 38, 16, 2, 46, 32, 14, 2, 53, 37, 15, 2, 50, 33, 14, 2, 70, 32, 47, 14, 64, 32, 45, 15, 69,
    31, 49, 15, 55, 23, 40, 13, 65, 28, 46, 15, 57, 28, 45, 13, 63, 33, 47, 16, 49, 24, 33, 10, 66, 29, 46, 13, 52,
    27, 39, 14, 50, 20, 35, 10, 59, 30, 42, 15, 60, 22, 40, 10, 61, 29, 47, 14, 56, 29, 36, 13, 67, 31, 44, 14, 56,
    30, 45, 15, 58, 27, 41, 10, 62, 22, 45, 15, 56, 25, 39, 11, 59, 32, 48, 18, 61, 28, 40, 13, 63, 25, 49, 15, 61,
    28, 47, 12, 64, 29, 43, 13, 66, 30, 44, 14, 68, 28, 48, 14, 67, 30, 50, 17, 60, 29, 45, 15, 57, 26, 35, 10, 55,
    24, 38, 11, 55, 24, 37, 10, 58, 27, 39, 12, 60, 27, 51, 16, 54, 30, 45, 15, 60, 34, 45, 16, 67, 31, 47, 15, 63,
    23, 44, 13, 56, 30, 41, 13, 55, 25, 40, 13, 55, 26, 44, 12, 61, 30, 46, 14, 58, 26, 40, 12, 50, 23, 33, 10, 56,
    27, 42, 13, 57, 30, 42, 12, 57, 29, 42, 13, 62, 29, 43, 13, 51, 25, 30, 11, 57, 28, 41, 13, 63, 33, 60, 25, 58,
    27, 51, 19, 71, 30, 59, 21, 63, 29, 56, 18, 65, 30, 58, 22, 76, 30, 66, 21, 49, 25, 45, 17, 73, 29, 63, 18, 67,
    25, 58, 18, 72, 36, 61, 25, 65, 32, 51, 20, 64, 27, 53, 19, 68, 30, 55, 21, 57, 25, 50, 20, 58, 28, 51, 24, 64,
    32, 53, 23, 65, 30, 55, 18, 77, 38, 67, 22, 77, 26, 69, 23, 60, 22, 50, 15, 69, 32, 57, 23, 56, 28, 49, 20, 77,
    28, 67, 20, 63, 27, 49, 18, 67, 33, 57, 21, 72, 32, 60, 18, 62, 28, 48, 18, 61, 30, 49, 18, 64, 28, 56, 21, 72,
    30, 58, 16, 74, 28, 61, 19, 79, 38, 64, 20, 64, 28, 56, 22, 63, 28, 51, 15, 61, 26, 56, 14, 77, 30, 61, 23, 63,
    34, 56, 24, 64, 31, 55, 18, 60, 30, 48, 18, 69, 31, 54, 21, 67, 31, 56, 24, 69, 31, 51, 23, 58, 27, 51, 19, 68,
    32, 59, 23, 67, 33, 57, 25, 67, 30, 52, 23, 63, 25, 50, 19, 65, 30, 52, 20, 62, 34, 54, 23, 59, 30, 51, 18,
};

static LubeValue str_list(LubeVM *vm, const char *const *names, int n) {
    LubeValue list = lube->new_list(vm);
    for (int i = 0; i < n; i++) lube->list_push(list, lube->new_str(vm, names[i], strlen(names[i])));
    return list;
}

/* [X, y] (y as int64), or [X, y, extra] if extra is not nothing. */
static LubeValue x_y(LubeVM *vm, LubeValue x, LubeValue y, LubeValue extra) {
    LubeValue list = lube->new_list(vm);
    lube->list_push(list, x);
    lube->list_push(list, y);
    if (lube->kind(extra) != LUBE_NOTHING) lube->list_push(list, extra);
    return list;
}

/* load_iris({return_X_y}): {data, target, feature_names, target_names}, or [X, y]. */
FN(f_load_iris) {
    LubeValue opts, err;
    int xy;
    if (!options_in(vm, "load_iris", argc, argv, 0, "return_X_y", &opts, &err) ||
        !opt_bool(vm, "load_iris", opts, "return_X_y", 0, &xy, &err))
        return err;
    double *x;
    int64_t *y;
    LubeValue xv = new_f64(vm, 150, 4, &x), yv = new_i64(vm, 150, -1, &y);
    for (int i = 0; i < 600; i++) x[i] = IRIS[i] / 10.0;
    for (int i = 0; i < 150; i++) y[i] = i / 50;
    if (xy) return x_y(vm, xv, yv, lube->nothing());
    static const char *const features[] = {"sepal length (cm)", "sepal width (cm)", "petal length (cm)", "petal width (cm)"};
    static const char *const species[] = {"setosa", "versicolor", "virginica"};
    LubeValue o = lube->new_object(vm);
    lube->object_set(vm, o, "data", xv);
    lube->object_set(vm, o, "target", yv);
    lube->object_set(vm, o, "feature_names", str_list(vm, features, 4));
    lube->object_set(vm, o, "target_names", str_list(vm, species, 3));
    return o;
}

/* The rows of x (n x d) and of y (n x t) in the order of a random permutation. */
static void shuffle_rows(Mt *rng, double *x, int64_t d, double *y, int64_t t, int64_t n) {
    int64_t *perm = (int64_t *)alloc(sizeof(int64_t) * (size_t)n);
    double *tmp = (double *)alloc(sizeof(double) * (size_t)(n * (d > t ? d : t)));
    for (int64_t i = 0; i < n; i++) perm[i] = i;
    mt_shuffle(rng, perm, n);
    for (int64_t i = 0; i < n; i++) memcpy(tmp + i * d, x + perm[i] * d, sizeof(double) * (size_t)d);
    memcpy(x, tmp, sizeof(double) * (size_t)(n * d));
    for (int64_t i = 0; i < n; i++) memcpy(tmp + i * t, y + perm[i] * t, sizeof(double) * (size_t)t);
    memcpy(y, tmp, sizeof(double) * (size_t)(n * t));
    free(perm);
    free(tmp);
}

/* An int64 Array of n labels (doubles). */
static LubeValue labels_i64(LubeVM *vm, const double *y, int64_t n) {
    int64_t *out;
    LubeValue r = new_i64(vm, n, -1, &out);
    for (int64_t i = 0; i < n; i++) out[i] = (int64_t)y[i];
    return r;
}

/* The common options: n_samples (whole, 1 or more), shuffle, random_state (the generator's seed: *rng, to free). */
static int sample_opts(LubeVM *vm, const char *fname, LubeValue opts, double fallback, double *n, int *shuffle, Mt **rng,
                       LubeValue *err) {
    double seed;
    if (!opt_number(vm, fname, opts, "n_samples", fallback, 1, 1e9, 1, n, err) ||
        !opt_bool(vm, fname, opts, "shuffle", 1, shuffle, err) ||
        !opt_number(vm, fname, opts, "random_state", -1, 0, 4294967295.0, 1, &seed, err))
        return 0;
    *rng = mt_for(seed);
    return 1;
}

/* make_blobs({n_samples, n_features, centers, cluster_std, center_box, shuffle, random_state}): [X, y], blobs of
 * normal samples round centers (a count, drawn in center_box, or the points themselves). */
FN(f_make_blobs) {
    LubeValue opts, err, cv, sv;
    double ns, nf, lo = -10, hi = 10;
    int shuffle;
    if (!options_in(vm, "make_blobs", argc, argv, 0, "n_samples|n_features|centers|cluster_std|center_box|shuffle|random_state", &opts, &err) ||
        !opt_number(vm, "make_blobs", opts, "n_features", 2, 1, 1e6, 1, &nf, &err))
        return err;
    if (opt(vm, opts, "center_box", &cv)) {
        if (lube->kind(cv) != LUBE_LIST || lube->list_len(cv) != 2 || lube->kind(lube->list_get(cv, 0)) != LUBE_NUMBER ||
            lube->kind(lube->list_get(cv, 1)) != LUBE_NUMBER || !(lube->number(lube->list_get(cv, 0)) < lube->number(lube->list_get(cv, 1))))
            return lube->error(vm, "make_blobs(): center_box must be a list [low, high] with low below high");
        lo = lube->number(lube->list_get(cv, 0));
        hi = lube->number(lube->list_get(cv, 1));
    }
    Data given = NO_DATA;
    given.keep = lube->nothing();
    int64_t k = 3, d = (int64_t)nf;
    if (opt(vm, opts, "centers", &cv)) {
        if (lube->kind(cv) == LUBE_NUMBER) {
            double c = lube->number(cv);
            if (c < 1 || c != floor(c) || c > 1e6) return lube->error(vm, "make_blobs(): centers must be a whole Number, 1 or more, or the centers");
            k = (int64_t)c;
        } else {
            if (!data_in(vm, "make_blobs", "centers", cv, 0, W_MATRIX, &given, &err)) return err;
            k = given.n;
            d = given.d;
        }
    }
    double *stds = (double *)alloc(sizeof(double) * (size_t)k);
    for (int64_t c = 0; c < k; c++) stds[c] = 1;
    if (opt(vm, opts, "cluster_std", &sv)) {
        int ok = lube->kind(sv) == LUBE_NUMBER || (lube->kind(sv) == LUBE_LIST && (int64_t)lube->list_len(sv) == k);
        for (int64_t c = 0; ok && c < k; c++) {
            LubeValue e = lube->kind(sv) == LUBE_LIST ? lube->list_get(sv, (size_t)c) : sv;
            ok = lube->kind(e) == LUBE_NUMBER && lube->number(e) >= 0;
            stds[c] = lube->number(e);
        }
        if (!ok) {
            free(stds);
            data_free(&given);
            return lube->error(vm, "make_blobs(): cluster_std must be a Number, 0 or more, or a list of one per center");
        }
    }
    Mt *rng;
    if (!sample_opts(vm, "make_blobs", opts, 100, &ns, &shuffle, &rng, &err)) {
        free(stds);
        data_free(&given);
        return err;
    }
    int64_t n = (int64_t)ns;
    double *centers = (double *)alloc(sizeof(double) * (size_t)(k * d)), *x, *y = (double *)alloc(sizeof(double) * (size_t)n);
    if (given.v) memcpy(centers, given.v, sizeof(double) * (size_t)(k * d));
    else mt_fill(rng, centers, k * d, 0, lo, hi);
    data_free(&given);
    LubeValue xv = new_f64(vm, n, d, &x);
    for (int64_t c = 0, at = 0; c < k; c++) {
        int64_t count = n / k + (c < n % k);
        mt_fill(rng, x + at * d, count * d, 1, 0, 1);
        for (int64_t i = at; i < at + count; i++) {
            y[i] = (double)c;
            for (int64_t f = 0; f < d; f++) x[i * d + f] = centers[c * d + f] + stds[c] * x[i * d + f];
        }
        at += count;
    }
    if (shuffle) shuffle_rows(rng, x, d, y, 1, n);
    LubeValue r = x_y(vm, xv, labels_i64(vm, y, n), lube->nothing());
    free(rng);
    free(centers);
    free(stds);
    free(y);
    return r;
}

/* make_moons({n_samples, noise, shuffle, random_state}): [X, y], two interleaving half circles. */
FN(f_make_moons) {
    LubeValue opts, err;
    double ns, noise;
    int shuffle;
    Mt *rng;
    if (!options_in(vm, "make_moons", argc, argv, 0, "n_samples|noise|shuffle|random_state", &opts, &err) ||
        !opt_number(vm, "make_moons", opts, "noise", 0, 0, INFINITY, 0, &noise, &err) ||
        !sample_opts(vm, "make_moons", opts, 100, &ns, &shuffle, &rng, &err))
        return err;
    int64_t n = (int64_t)ns, n_out = n / 2, n_in = n - n_out;
    double *x, *y = (double *)alloc(sizeof(double) * (size_t)n);
    LubeValue xv = new_f64(vm, n, 2, &x);
    for (int64_t i = 0; i < n; i++) {
        int outer = i < n_out;
        int64_t j = outer ? i : i - n_out, m = outer ? n_out : n_in;
        double a = m > 1 ? 3.141592653589793 * (double)j / (double)(m - 1) : 0;
        x[2 * i] = outer ? cos(a) : 1 - cos(a);
        x[2 * i + 1] = outer ? sin(a) : 1 - sin(a) - 0.5;
        y[i] = !outer;
    }
    if (shuffle) shuffle_rows(rng, x, 2, y, 1, n);
    if (noise > 0) {
        double *z = (double *)alloc(sizeof(double) * (size_t)(2 * n));
        mt_fill(rng, z, 2 * n, 1, 0, noise);
        for (int64_t i = 0; i < 2 * n; i++) x[i] += z[i];
        free(z);
    }
    LubeValue r = x_y(vm, xv, labels_i64(vm, y, n), lube->nothing());
    free(rng);
    free(y);
    return r;
}

/* make_regression({n_samples, n_features, n_informative, n_targets, bias, noise, shuffle, coef, random_state}):
 * [X, y] (or [X, y, coef]): y a linear function of n_informative of the normal features, plus noise. */
FN(f_make_regression) {
    LubeValue opts, err;
    double ns, nf, ni, nt, bias, noise;
    int shuffle, want_coef;
    Mt *rng;
    if (!options_in(vm, "make_regression", argc, argv, 0,
                    "n_samples|n_features|n_informative|n_targets|bias|noise|shuffle|coef|random_state", &opts, &err) ||
        !opt_number(vm, "make_regression", opts, "n_features", 100, 1, 1e6, 1, &nf, &err) ||
        !opt_number(vm, "make_regression", opts, "n_informative", 10, 0, 1e6, 1, &ni, &err) ||
        !opt_number(vm, "make_regression", opts, "n_targets", 1, 1, 1e6, 1, &nt, &err) ||
        !opt_number(vm, "make_regression", opts, "bias", 0, -INFINITY, INFINITY, 0, &bias, &err) ||
        !opt_number(vm, "make_regression", opts, "noise", 0, 0, INFINITY, 0, &noise, &err) ||
        !opt_bool(vm, "make_regression", opts, "coef", 0, &want_coef, &err) ||
        !sample_opts(vm, "make_regression", opts, 100, &ns, &shuffle, &rng, &err))
        return err;
    int64_t n = (int64_t)ns, d = (int64_t)nf, t = (int64_t)nt, inf = ni < nf ? (int64_t)ni : d;
    double *x, *y, *gt = (double *)alloc(sizeof(double) * (size_t)(d * t)), *yy = (double *)alloc(sizeof(double) * (size_t)(n * t));
    LubeValue xv = new_f64(vm, n, d, &x);
    mt_fill(rng, x, n * d, 1, 0, 1);
    mt_fill(rng, gt, inf * t, 0, 0, 100);
    for (int64_t i = 0; i < n; i++)
        for (int64_t o = 0; o < t; o++) {
            double s = bias;
            for (int64_t f = 0; f < inf; f++) s += x[i * d + f] * gt[f * t + o];
            yy[i * t + o] = s;
        }
    if (noise > 0) {
        double *z = (double *)alloc(sizeof(double) * (size_t)(n * t));
        mt_fill(rng, z, n * t, 1, 0, noise);
        for (int64_t i = 0; i < n * t; i++) yy[i] += z[i];
        free(z);
    }
    if (shuffle) {  /* the samples, then the features (with their coefficients) */
        shuffle_rows(rng, x, d, yy, t, n);
        int64_t *perm = (int64_t *)alloc(sizeof(int64_t) * (size_t)d);
        double *row = (double *)alloc(sizeof(double) * (size_t)(d * (t > 1 ? t : 1) + d)), *g2 = row + d;
        for (int64_t f = 0; f < d; f++) perm[f] = f;
        mt_shuffle(rng, perm, d);
        for (int64_t i = 0; i < n; i++) {
            for (int64_t f = 0; f < d; f++) row[f] = x[i * d + perm[f]];
            memcpy(x + i * d, row, sizeof(double) * (size_t)d);
        }
        for (int64_t f = 0; f < d; f++)
            for (int64_t o = 0; o < t; o++) g2[f * t + o] = gt[perm[f] * t + o];
        memcpy(gt, g2, sizeof(double) * (size_t)(d * t));
        free(perm);
        free(row);
    }
    LubeValue yv = new_f64(vm, n, t > 1 ? t : -1, &y), cv = lube->nothing();
    memcpy(y, yy, sizeof(double) * (size_t)(n * t));
    if (want_coef) {
        double *c;
        cv = new_f64(vm, d, t > 1 ? t : -1, &c);
        memcpy(c, gt, sizeof(double) * (size_t)(d * t));
    }
    free(rng);
    free(gt);
    free(yy);
    return x_y(vm, xv, yv, cv);
}

/* make_classification({n_samples, n_features, n_informative, n_redundant, n_classes, n_clusters_per_class,
 * class_sep, flip_y, shuffle, random_state}): [X, y], sklearn's way: normal clusters (each with a random
 * covariance) at corners of a hypercube in the informative features, redundant features mixing those, the rest
 * noise, and a share flip_y of the labels at random. */
FN(f_make_classification) {
    LubeValue opts, err;
    double ns, nf, ni, nr, nc, ncl, sep, flip;
    int shuffle;
    Mt *rng = NULL;
    if (!options_in(vm, "make_classification", argc, argv, 0,
                    "n_samples|n_features|n_informative|n_redundant|n_classes|n_clusters_per_class|class_sep|flip_y|shuffle|random_state",
                    &opts, &err) ||
        !opt_number(vm, "make_classification", opts, "n_features", 20, 1, 1e6, 1, &nf, &err) ||
        !opt_number(vm, "make_classification", opts, "n_informative", 2, 1, 30, 1, &ni, &err) ||
        !opt_number(vm, "make_classification", opts, "n_redundant", 2, 0, 1e6, 1, &nr, &err) ||
        !opt_number(vm, "make_classification", opts, "n_classes", 2, 1, 1e6, 1, &nc, &err) ||
        !opt_number(vm, "make_classification", opts, "n_clusters_per_class", 2, 1, 1e6, 1, &ncl, &err) ||
        !opt_number(vm, "make_classification", opts, "class_sep", 1, 0, INFINITY, 0, &sep, &err) ||
        !opt_number(vm, "make_classification", opts, "flip_y", 0.01, 0, 1, 0, &flip, &err) ||
        !sample_opts(vm, "make_classification", opts, 100, &ns, &shuffle, &rng, &err))
        return err;
    if (ni + nr > nf || nc * ncl > pow(2, ni)) {
        free(rng);
        return lube->error(vm, ni + nr > nf ? "make_classification(): n_informative + n_redundant must be at most n_features"
                                            : "make_classification(): n_classes * n_clusters_per_class must be at most 2^n_informative");
    }
    int64_t n = (int64_t)ns, d = (int64_t)nf, inf = (int64_t)ni, red = (int64_t)nr, k = (int64_t)nc, clusters = (int64_t)(nc * ncl);
    double *x, *y = (double *)alloc(sizeof(double) * (size_t)n), *a = (double *)alloc(sizeof(double) * (size_t)(inf * (inf > red ? inf : red)));
    double *row = (double *)alloc(sizeof(double) * (size_t)inf);
    int64_t *corner = (int64_t *)alloc(sizeof(int64_t) * (size_t)clusters);
    LubeValue xv = new_f64(vm, n, d, &x);
    for (int64_t c = 0; c < clusters; c++) {  /* different corners, at random */
        int fresh;
        do {
            corner[c] = (int64_t)(mt_double(rng) * pow(2, ni));
            fresh = 1;
            for (int64_t q = 0; q < c; q++) fresh &= corner[q] != corner[c];
        } while (!fresh);
    }
    double *z = (double *)alloc(sizeof(double) * (size_t)(n * inf));
    mt_fill(rng, z, n * inf, 1, 0, 1);
    for (int64_t c = 0, at = 0; c < clusters; c++) {
        int64_t count = n / clusters + (c < n % clusters);
        mt_fill(rng, a, inf * inf, 0, -1, 1);
        for (int64_t i = at; i < at + count; i++) {
            y[i] = (double)(c % k);
            for (int64_t f = 0; f < inf; f++) {
                double s = ((corner[c] >> f) & 1 ? sep : -sep);
                for (int64_t g = 0; g < inf; g++) s += z[i * inf + g] * a[g * inf + f];
                x[i * d + f] = s;
            }
        }
        at += count;
    }
    mt_fill(rng, a, inf * red, 0, -1, 1);
    for (int64_t i = 0; i < n; i++) {
        for (int64_t f = 0; f < red; f++) {
            double s = 0;
            for (int64_t g = 0; g < inf; g++) s += x[i * d + g] * a[g * red + f];
            x[i * d + inf + f] = s;
        }
    }
    if (d > inf + red) {
        double *noise = (double *)alloc(sizeof(double) * (size_t)(n * (d - inf - red)));
        mt_fill(rng, noise, n * (d - inf - red), 1, 0, 1);
        for (int64_t i = 0; i < n; i++)
            for (int64_t f = inf + red; f < d; f++) x[i * d + f] = noise[i * (d - inf - red) + f - inf - red];
        free(noise);
    }
    if (flip > 0) {
        double *u = (double *)alloc(sizeof(double) * (size_t)n);
        mt_fill(rng, u, n, 0, 0, 1);
        for (int64_t i = 0; i < n; i++)
            if (u[i] < flip) y[i] = (double)mt_below(rng, (uint32_t)(k - 1));
        free(u);
    }
    if (shuffle) {
        shuffle_rows(rng, x, d, y, 1, n);
        int64_t *perm = (int64_t *)alloc(sizeof(int64_t) * (size_t)d);
        double *tmp = (double *)alloc(sizeof(double) * (size_t)d);
        for (int64_t f = 0; f < d; f++) perm[f] = f;
        mt_shuffle(rng, perm, d);
        for (int64_t i = 0; i < n; i++) {
            for (int64_t f = 0; f < d; f++) tmp[f] = x[i * d + perm[f]];
            memcpy(x + i * d, tmp, sizeof(double) * (size_t)d);
        }
        free(perm);
        free(tmp);
    }
    LubeValue r = x_y(vm, xv, labels_i64(vm, y, n), lube->nothing());
    free(rng);
    free(y);
    free(a);
    free(row);
    free(corner);
    free(z);
    return r;
}
