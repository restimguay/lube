/*
 * learn/metrics.c - how good predictions are: for classes (accuracy,
 * precision, recall, F-scores, the confusion matrix, a report, log loss,
 * ROC AUC), for numbers (squared and absolute errors, R^2) and for clusters
 * (silhouette, adjusted Rand index). (Part of learn.c.)
 */

/* ======================================================================
 * Options of functions (not models): checked against what they take
 * ==================================================================== */

/* fname's options object (argument i, if there is one): 0 with *err if it is not an object or has a name that is
 * not among names ("a|b|c"). */
static int options_in(LubeVM *vm, const char *fname, int argc, const LubeValue *argv, int i, const char *names,
                      LubeValue *opts, LubeValue *err) {
    *opts = lube->nothing();
    if (argc <= i) return 1;
    if (lube->kind(argv[i]) != LUBE_OBJECT) {
        *err = lube->error(vm, "%s() takes its options as an object (argument %d), got %s", fname, i + 1, lube->a_type(argv[i]));
        return 0;
    }
    *opts = argv[i];
    LubeValue keys = lube->object_keys(vm, argv[i]);
    int ok = 1;
    for (size_t k = 0; ok && k < lube->list_len(keys); k++) {
        const char *key = lube->str(lube->list_get(keys, k), NULL), *s = names;
        size_t len = strlen(key);
        for (;;) {
            size_t n = strcspn(s, "|");
            if (n == len && !strncmp(s, key, n)) break;
            if (!s[n]) {
                char list[256];
                size_t at = 0;
                for (const char *p = names; *p; p++) {
                    if (at + 3 >= sizeof list) break;  /* LCOV_EXCL_LINE: (the lists are short) */
                    if (*p == '|') {
                        list[at++] = ',';
                        list[at++] = ' ';
                    } else list[at++] = *p;
                }
                list[at] = 0;
                *err = lube->error(vm, "%s(): there is no option \"%s\" (it has %s)", fname, key, list);
                ok = 0;
                break;
            }
            s += n + 1;
        }
    }
    lube->release(keys);
    return ok;
}

/* An option's value (borrowed), if it is there. */
static int opt(LubeVM *vm, LubeValue opts, const char *key, LubeValue *out) {
    return lube->kind(opts) == LUBE_OBJECT && lube->object_get(vm, opts, key, out);
}

/* A Number option: fallback if absent; 0 with *err if it is not a Number from lo to hi (whole if asked). */
static int opt_number(LubeVM *vm, const char *fname, LubeValue opts, const char *key, double fallback, double lo,
                      double hi, int whole, double *out, LubeValue *err) {
    LubeValue v;
    *out = fallback;
    if (!opt(vm, opts, key, &v)) return 1;
    double x = lube->number(v);
    if (lube->kind(v) != LUBE_NUMBER || !(x >= lo && x <= hi) || (whole && x != floor(x))) {
        char got[128];
        *err = lube->error(vm, "%s(): %s must be a %sNumber from %g to %g, not %s", fname, key, whole ? "whole " : "", lo, hi,
                           value_text(v, got, sizeof got));
        return 0;
    }
    *out = x;
    return 1;
}

static int opt_bool(LubeVM *vm, const char *fname, LubeValue opts, const char *key, int fallback, int *out, LubeValue *err) {
    LubeValue v;
    *out = fallback;
    if (!opt(vm, opts, key, &v)) return 1;
    if (lube->kind(v) != LUBE_BOOL) {
        char got[128];
        *err = lube->error(vm, "%s(): %s must be true or false, not %s", fname, key, value_text(v, got, sizeof got));
        return 0;
    }
    *out = lube->truthy(v);
    return 1;
}

/* A text option among choices ("a|b|c"): its index, fallback if absent; -1 with *err. */
static int opt_choice(LubeVM *vm, const char *fname, LubeValue opts, const char *key, const char *choices, int fallback,
                      LubeValue *err) {
    LubeValue v;
    if (!opt(vm, opts, key, &v)) return fallback;
    const char *s = lube->str(v, NULL), *c = choices;
    for (int i = 0; s; i++) {
        size_t n = strcspn(c, "|");
        if (n == strlen(s) && !strncmp(c, s, n)) return i;
        if (!c[n]) break;
        c += n + 1;
    }
    char got[128], list[512];
    size_t at = 0;
    for (const char *p = choices; *p && at + 3 < sizeof list; p++) {
        if (*p == '|') {
            list[at++] = ',';
            list[at++] = ' ';
        } else list[at++] = *p;
    }
    list[at] = 0;
    *err = lube->error(vm, "%s(): %s must be one of %s; not %s", fname, key, list, value_text(v, got, sizeof got));
    return -1;
}

/* y_true and y_pred (arguments 0 and 1) as vectors of as many values. */
static int pair_in(LubeVM *vm, const char *fname, const LubeValue *argv, int want, Data *t, Data *p, LubeValue *err) {
    if (!data_in(vm, fname, "y_true", argv[0], 0, want, t, err)) return 0;
    if (!data_in(vm, fname, "y_pred", argv[1], 1, want, p, err)) {
        data_free(t);
        return 0;
    }
    if (t->n != p->n || t->d != p->d) {
        *err = lube->error(vm, "%s(): y_true has %.0f values and y_pred %.0f", fname, (double)(t->n * t->d), (double)(p->n * p->d));
        data_free(t);
        data_free(p);
        return 0;
    }
    return 1;
}

/* The labels: those of the option `labels` if given, else the values of y_true and y_pred, sorted. */
static double *labels_of(LubeVM *vm, const char *fname, LubeValue opts, const Data *t, const Data *p, int64_t *k,
                         LubeValue *err) {
    LubeValue v;
    if (opt(vm, opts, "labels", &v)) {
        Data l = NO_DATA;
        if (!data_in(vm, fname, "labels", v, 2, W_VECTOR, &l, err)) return NULL;
        double *u = (double *)alloc(sizeof(double) * (size_t)l.n);
        memcpy(u, l.v, sizeof(double) * (size_t)l.n);
        *k = l.n;
        data_free(&l);
        return u;
    }
    double *both = (double *)alloc(sizeof(double) * (size_t)(t->n + p->n));
    memcpy(both, t->v, sizeof(double) * (size_t)t->n);
    memcpy(both + t->n, p->v, sizeof(double) * (size_t)p->n);
    double *u = distinct(both, t->n + p->n, k);
    free(both);
    return u;
}

/* The index of x among the k labels (not sorted when given), or -1. */
static int64_t label_index(const double *labels, int64_t k, double x) {
    for (int64_t i = 0; i < k; i++)
        if (labels[i] == x) return i;
    return -1;
}

/* ======================================================================
 * Classes
 * ==================================================================== */

FN(f_accuracy_score) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue opts, err;
    int normalize;
    if (!options_in(vm, "accuracy_score", argc, argv, 2, "normalize", &opts, &err) ||
        !opt_bool(vm, "accuracy_score", opts, "normalize", 1, &normalize, &err) ||
        !pair_in(vm, "accuracy_score", argv, W_VECTOR, &t, &p, &err))
        return err;
    double a = accuracy_of(t.v, p.v, t.n);
    if (!normalize) a = floor(a * (double)t.n + 0.5);
    data_free(&t);
    data_free(&p);
    return lube->number_value(a);
}

/* Each label's true positives, false positives and false negatives (k x 3), for the k labels. */
static double *counts_of(const Data *t, const Data *p, const double *labels, int64_t k) {
    double *c = (double *)alloc(sizeof(double) * (size_t)(k * 3));
    for (int64_t i = 0; i < t->n; i++) {
        int64_t a = label_index(labels, k, t->v[i]), b = label_index(labels, k, p->v[i]);
        if (a >= 0 && a == b) c[a * 3]++;
        else {
            if (b >= 0) c[b * 3 + 1]++;
            if (a >= 0) c[a * 3 + 2]++;
        }
    }
    return c;
}

static double ratio(double a, double b, double zero_division) { return b == 0 ? zero_division : a / b; }

/* precision (what 0), recall (1) or the F-score with beta (2) from tp, fp, fn. */
static double prf_one(int what, double tp, double fp, double fn, double beta, double zd) {
    if (what == 0) return ratio(tp, tp + fp, zd);
    if (what == 1) return ratio(tp, tp + fn, zd);
    double b2 = beta * beta;
    return ratio((1 + b2) * tp, (1 + b2) * tp + b2 * fn + fp, zd);
}

enum { AV_BINARY, AV_MICRO, AV_MACRO, AV_WEIGHTED, AV_NONE };

/* precision_score, recall_score and the F-scores: a Number, or each label's (average "none"). */
static LubeValue prf(LubeVM *vm, const char *fname, int what, int argc, const LubeValue *argv, int opts_at, double beta) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue opts, err, v;
    double zd, pos_label = 1;
    if (!options_in(vm, fname, argc, argv, opts_at, "average|pos_label|labels|zero_division", &opts, &err) ||
        !opt_number(vm, fname, opts, "zero_division", 0, 0, 1, 0, &zd, &err))
        return err;
    int average = opt_choice(vm, fname, opts, "average", "binary|micro|macro|weighted|none", AV_BINARY, &err);
    if (average < 0) return err;
    if (opt(vm, opts, "pos_label", &v)) {
        if (lube->kind(v) != LUBE_NUMBER) return lube->error(vm, "%s(): pos_label must be a Number, not %s", fname, lube->a_type(v));
        pos_label = lube->number(v);
    }
    if (!pair_in(vm, fname, argv, W_VECTOR, &t, &p, &err)) return err;
    int64_t k;
    double *labels = labels_of(vm, fname, opts, &t, &p, &k, &err);
    LubeValue r = labels ? lube->nothing() : err;
    if (!labels) goto done;
    if (average == AV_BINARY) {
        if (k > 2) {
            r = lube->error(vm, "%s(): y has %.0f classes, but average is \"binary\": give average \"micro\", "
                                "\"macro\", \"weighted\" or \"none\"", fname, (double)k);
            goto done;
        }
        if (label_index(labels, k, pos_label) < 0 && k == 2) {
            char num[64];
            lube->format_number(pos_label, num, sizeof num);
            r = lube->error(vm, "%s(): pos_label %s is not one of the labels", fname, num);
            goto done;
        }
        free(labels);
        labels = (double *)alloc(sizeof(double));
        labels[0] = pos_label;
        k = 1;
    }
    double *c = counts_of(&t, &p, labels, k);
    if (average == AV_NONE) {
        double *out;
        r = new_f64(vm, k, -1, &out);
        for (int64_t i = 0; i < k; i++) out[i] = prf_one(what, c[i * 3], c[i * 3 + 1], c[i * 3 + 2], beta, zd);
    } else if (average == AV_MICRO) {
        double tp = 0, fp = 0, fn = 0;
        for (int64_t i = 0; i < k; i++) {
            tp += c[i * 3];
            fp += c[i * 3 + 1];
            fn += c[i * 3 + 2];
        }
        r = lube->number_value(prf_one(what, tp, fp, fn, beta, zd));
    } else {
        double s = 0, total = 0;
        for (int64_t i = 0; i < k; i++) {
            double w = average == AV_WEIGHTED ? c[i * 3] + c[i * 3 + 2] : 1;
            s += w * prf_one(what, c[i * 3], c[i * 3 + 1], c[i * 3 + 2], beta, zd);
            total += w;
        }
        r = lube->number_value(total == 0 ? zd : s / total);
    }
    free(c);
done:
    free(labels);
    data_free(&t);
    data_free(&p);
    return r;
}

FN(f_precision_score) { return prf(vm, "precision_score", 0, argc, argv, 2, 1); }
FN(f_recall_score) { return prf(vm, "recall_score", 1, argc, argv, 2, 1); }
FN(f_f1_score) { return prf(vm, "f1_score", 2, argc, argv, 2, 1); }

FN(f_fbeta_score) {
    if (lube->kind(argv[2]) != LUBE_NUMBER || !(lube->number(argv[2]) > 0))
        return lube->error(vm, "fbeta_score(): beta (argument 3) must be a Number more than 0");
    return prf(vm, "fbeta_score", 2, argc, argv, 3, lube->number(argv[2]));
}

/* balanced_accuracy_score: the mean recall of the classes in y_true. */
FN(f_balanced_accuracy_score) {
    (void)argc;
    Data t = NO_DATA, p = NO_DATA;
    LubeValue err;
    if (!pair_in(vm, "balanced_accuracy_score", argv, W_VECTOR, &t, &p, &err)) return err;
    int64_t k;
    double *labels = distinct(t.v, t.n, &k), *c = counts_of(&t, &p, labels, k), s = 0;
    for (int64_t i = 0; i < k; i++) s += c[i * 3] / (c[i * 3] + c[i * 3 + 2]);
    free(labels);
    free(c);
    data_free(&t);
    data_free(&p);
    return lube->number_value(s / (double)k);
}

FN(f_confusion_matrix) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue opts, err;
    if (!options_in(vm, "confusion_matrix", argc, argv, 2, "labels", &opts, &err) ||
        !pair_in(vm, "confusion_matrix", argv, W_VECTOR, &t, &p, &err))
        return err;
    int64_t k, *out;
    double *labels = labels_of(vm, "confusion_matrix", opts, &t, &p, &k, &err);
    LubeValue r = labels ? lube->nothing() : err;
    if (labels) {
        r = new_i64(vm, k, k, &out);
        for (int64_t i = 0; i < t.n; i++) {
            int64_t a = label_index(labels, k, t.v[i]), b = label_index(labels, k, p.v[i]);
            if (a >= 0 && b >= 0) out[a * k + b]++;
        }
        free(labels);
    }
    data_free(&t);
    data_free(&p);
    return r;
}

/* A growing text. */
typedef struct {
    char *s;
    size_t len, cap;
} Text;

static void text_add(Text *t, const char *format, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0) return;  /* LCOV_EXCL_LINE: (an encoding error: there are none in these formats) */
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;  /* LCOV_EXCL_LINE: (lines are short) */
    if (t->len + (size_t)n + 1 > t->cap) {
        t->cap = (t->len + (size_t)n + 1) * 2;
        t->s = (char *)realloc(t->s, t->cap);
        if (!t->s) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    memcpy(t->s + t->len, buf, (size_t)n + 1);
    t->len += (size_t)n;
}

/* classification_report(y_true, y_pred, {digits, target_names}): sklearn's table, as text. */
FN(f_classification_report) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue opts, err, names = lube->nothing();
    double digits;
    if (!options_in(vm, "classification_report", argc, argv, 2, "digits|target_names|labels", &opts, &err) ||
        !opt_number(vm, "classification_report", opts, "digits", 2, 0, 15, 1, &digits, &err) ||
        !pair_in(vm, "classification_report", argv, W_VECTOR, &t, &p, &err))
        return err;
    int64_t k;
    double *labels = labels_of(vm, "classification_report", opts, &t, &p, &k, &err);
    if (!labels) {
        data_free(&t);
        data_free(&p);
        return err;
    }
    if (opt(vm, opts, "target_names", &names) && (lube->kind(names) != LUBE_LIST || (int64_t)lube->list_len(names) != k)) {
        free(labels);
        data_free(&t);
        data_free(&p);
        return lube->error(vm, "classification_report(): target_names must be a list of %.0f names, one per label", (double)k);
    }
    char (*name)[64] = (char (*)[64])alloc(64 * (size_t)k);
    int width = 12, dg = (int)digits;
    for (int64_t i = 0; i < k; i++) {
        if (lube->kind(names) == LUBE_LIST) {
            LubeValue nv = lube->list_get(names, (size_t)i);
            if (lube->kind(nv) == LUBE_STR) snprintf(name[i], 64, "%s", lube->str(nv, NULL));
            else value_text(nv, name[i], 64);
        } else lube->format_number(labels[i], name[i], 64);
        if ((int)strlen(name[i]) > width) width = (int)strlen(name[i]);
    }
    if (dg > width) width = dg;
    double *c = counts_of(&t, &p, labels, k), sums[3][3] = {{0}}, total = 0, tp = 0, fp = 0, fn = 0;
    Text out = {NULL, 0, 0};
    text_add(&out, "%*s  %9s %9s %9s %9s\n\n", width, "", "precision", "recall", "f1-score", "support");
    for (int64_t i = 0; i < k; i++) {
        double row[3], support = c[i * 3] + c[i * 3 + 2];
        for (int w = 0; w < 3; w++) {
            row[w] = prf_one(w, c[i * 3], c[i * 3 + 1], c[i * 3 + 2], 1, 0);
            sums[0][w] += row[w];
            sums[1][w] += row[w] * support;
        }
        total += support;
        tp += c[i * 3];
        fp += c[i * 3 + 1];
        fn += c[i * 3 + 2];
        text_add(&out, "%*s  %9.*f %9.*f %9.*f %9.0f\n", width, name[i], dg, row[0], dg, row[1], dg, row[2], support);
    }
    text_add(&out, "\n");
    int is_accuracy = !opt(vm, opts, "labels", &err);
    if (is_accuracy) text_add(&out, "%*s  %9s %9s %9.*f %9.0f\n", width, "accuracy", "", "", dg, prf_one(0, tp, fp, fn, 1, 0), total);
    else text_add(&out, "%*s  %9.*f %9.*f %9.*f %9.0f\n", width, "micro avg", dg, prf_one(0, tp, fp, fn, 1, 0), dg,
                  prf_one(1, tp, fp, fn, 1, 0), dg, prf_one(2, tp, fp, fn, 1, 0), total);
    text_add(&out, "%*s  %9.*f %9.*f %9.*f %9.0f\n", width, "macro avg", dg, sums[0][0] / (double)k, dg,
             sums[0][1] / (double)k, dg, sums[0][2] / (double)k, total);
    text_add(&out, "%*s  %9.*f %9.*f %9.*f %9.0f\n", width, "weighted avg", dg, ratio(sums[1][0], total, 0), dg,
             ratio(sums[1][1], total, 0), dg, ratio(sums[1][2], total, 0), total);
    LubeValue r = lube->new_str(vm, out.s, out.len);
    free(out.s);
    free(name);
    free(c);
    free(labels);
    data_free(&t);
    data_free(&p);
    return r;
}

/* log_loss(y_true, y_proba, {labels}): the mean of -log(probability of the true class); probabilities clipped to
 * [eps, 1 - eps] and each row made to sum to 1. y_proba may be one column: the larger label's probability. */
FN(f_log_loss) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue opts, err;
    if (!options_in(vm, "log_loss", argc, argv, 2, "labels", &opts, &err)) return err;
    if (!data_in(vm, "log_loss", "y_true", argv[0], 0, W_VECTOR, &t, &err)) return err;
    if (!data_in(vm, "log_loss", "y_proba", argv[1], 1, W_EITHER, &p, &err)) {
        data_free(&t);
        return err;
    }
    int64_t k;
    double *labels = labels_of(vm, "log_loss", opts, &t, &t, &k, &err);
    LubeValue r = labels ? lube->nothing() : err;
    if (!labels) goto done;
    int64_t cols = p.nd == 2 ? p.d : 1;
    if (p.n != t.n || (cols != k && !(cols == 1 && k == 2)) || k < 2) {
        r = lube->error(vm, "log_loss(): y_true has %.0f samples of %.0f labels, but y_proba is %.0f x %.0f", (double)t.n,
                        (double)k, (double)p.n, (double)cols);
        goto done;
    }
    double s = 0, eps = DBL_EPSILON;
    for (int64_t i = 0; i < t.n; i++) {
        int64_t c = find_value(labels, k, t.v[i]);
        if (c < 0) {
            char num[64];
            lube->format_number(t.v[i], num, sizeof num);
            r = lube->error(vm, "log_loss(): y_true has %s, which is not one of the labels", num);
            goto done;
        }
        double row = 0, pc = 0;
        for (int64_t j = 0; j < k; j++) {
            double q = cols == 1 ? (j == 1 ? p.v[i] : 1 - p.v[i]) : p.v[i * cols + j];
            q = fmin(fmax(q, eps), 1 - eps);
            row += q;
            if (j == c) pc = q;
        }
        s -= log(pc / row);
    }
    r = lube->number_value(s / (double)t.n);
done:
    free(labels);
    data_free(&t);
    data_free(&p);
    return r;
}

/* The area under the ROC curve of scores for which `positive` says which samples are positive: the chance a
 * positive scores above a negative (ties half). */
static double auc_of(const double *score, const int *positive, int64_t n, int64_t stride) {
    int64_t *order = (int64_t *)alloc(sizeof(int64_t) * (size_t)n), np = 0;
    double *v = (double *)alloc(sizeof(double) * (size_t)n * 2);
    for (int64_t i = 0; i < n; i++) {  /* (sorted by score: pairs of score and index) */
        v[2 * i] = score[i * stride];
        v[2 * i + 1] = (double)i;
    }
    qsort(v, (size_t)n, sizeof(double) * 2, cmp_double);
    for (int64_t i = 0; i < n; i++) order[i] = (int64_t)v[2 * i + 1];
    double rank_sum = 0;
    for (int64_t i = 0; i < n;) {
        int64_t j = i;
        while (j < n && v[2 * j] == v[2 * i]) j++;
        double rank = (double)(i + j + 1) / 2;  /* (the mean rank of a run of ties, counting from 1) */
        for (int64_t q = i; q < j; q++)
            if (positive[order[q]]) {
                rank_sum += rank;
                np++;
            }
        i = j;
    }
    free(order);
    free(v);
    double nn = (double)(n - np);
    return (rank_sum - (double)np * (double)(np + 1) / 2) / ((double)np * nn);
}

/* roc_auc_score(y_true, y_score): two classes (y_score the larger's score), or more (y_score a column for each,
 * one against the rest, averaged). */
FN(f_roc_auc_score) {
    Data t = NO_DATA, s = NO_DATA;
    LubeValue opts, err;
    if (!options_in(vm, "roc_auc_score", argc, argv, 2, "multi_class|average", &opts, &err)) return err;
    if (opt_choice(vm, "roc_auc_score", opts, "multi_class", "ovr", 0, &err) < 0 ||
        opt_choice(vm, "roc_auc_score", opts, "average", "macro|weighted", 0, &err) < 0)
        return err;
    int weighted = opt_choice(vm, "roc_auc_score", opts, "average", "macro|weighted", 0, &err);
    if (!data_in(vm, "roc_auc_score", "y_true", argv[0], 0, W_VECTOR, &t, &err)) return err;
    if (!data_in(vm, "roc_auc_score", "y_score", argv[1], 1, W_EITHER, &s, &err)) {
        data_free(&t);
        return err;
    }
    int64_t k, cols = s.nd == 2 ? s.d : 1;
    double *labels = distinct(t.v, t.n, &k);
    LubeValue r;
    if (k < 2) r = lube->error(vm, "roc_auc_score(): y_true has only one class: the AUC is not defined");
    else if (s.n != t.n || (k == 2 ? cols > 2 : cols != k))
        r = lube->error(vm, "roc_auc_score(): y_true has %.0f samples of %.0f classes, but y_score is %.0f x %.0f",
                        (double)t.n, (double)k, (double)s.n, (double)cols);
    else {
        int *pos = (int *)alloc(sizeof(int) * (size_t)t.n);
        double total = 0, wsum = 0;
        for (int64_t c = k == 2 ? 1 : 0; c < k; c++) {
            double w = 0;
            for (int64_t i = 0; i < t.n; i++) w += pos[i] = t.v[i] == labels[c];
            double a = auc_of(s.v + (cols == 1 ? 0 : c), pos, t.n, cols);
            if (weighted && k > 2) a *= w;
            total += a;
            wsum += weighted && k > 2 ? w : 1;
        }
        free(pos);
        r = lube->number_value(total / wsum);
    }
    free(labels);
    data_free(&t);
    data_free(&s);
    return r;
}

/* ======================================================================
 * Numbers
 * ==================================================================== */

/* The mean over the outputs (columns) of what fn gives each. */
static LubeValue regression_metric(LubeVM *vm, const char *fname, const LubeValue *argv, int which) {
    Data t = NO_DATA, p = NO_DATA;
    LubeValue err;
    if (!pair_in(vm, fname, argv, W_EITHER, &t, &p, &err)) return err;
    int64_t n = t.n, o = t.nd == 2 ? t.d : 1;
    double total = 0;
    if (which == 3) total = r2_of(t.v, p.v, n, o) * (double)o;
    else
        for (int64_t c = 0; c < o; c++) {
            double s = 0, mean_t = 0, mean_e = 0, var_t = 0, var_e = 0;
            for (int64_t i = 0; i < n; i++) {
                double e = t.v[i * o + c] - p.v[i * o + c];
                s += which == 2 ? fabs(e) : e * e;
                mean_t += t.v[i * o + c];
                mean_e += e;
            }
            if (which == 4) {  /* explained variance: 1 - var(y - pred) / var(y) */
                mean_t /= (double)n;
                mean_e /= (double)n;
                for (int64_t i = 0; i < n; i++) {
                    double e = t.v[i * o + c] - p.v[i * o + c] - mean_e, u = t.v[i * o + c] - mean_t;
                    var_e += e * e;
                    var_t += u * u;
                }
                total += var_t == 0 ? (var_e == 0 ? 1.0 : 0.0) : 1 - var_e / var_t;
            } else total += which == 1 ? sqrt(s / (double)n) : s / (double)n;
        }
    data_free(&t);
    data_free(&p);
    return lube->number_value(total / (double)o);
}

FN(f_mean_squared_error) { (void)argc; return regression_metric(vm, "mean_squared_error", argv, 0); }
FN(f_root_mean_squared_error) { (void)argc; return regression_metric(vm, "root_mean_squared_error", argv, 1); }
FN(f_mean_absolute_error) { (void)argc; return regression_metric(vm, "mean_absolute_error", argv, 2); }
FN(f_r2_score) { (void)argc; return regression_metric(vm, "r2_score", argv, 3); }
FN(f_explained_variance_score) { (void)argc; return regression_metric(vm, "explained_variance_score", argv, 4); }

/* ======================================================================
 * Clusters
 * ==================================================================== */

typedef struct {
    const double *x;
    const int64_t *code;  /* each sample's cluster */
    const double *size;   /* each cluster's samples */
    int64_t n, d, k;
    double *s;            /* each sample's silhouette */
} SilJob;

static void silhouette_rows(void *ctx, int64_t from, int64_t to) {
    SilJob *j = (SilJob *)ctx;
    double *sum = (double *)alloc(sizeof(double) * (size_t)j->k);
    for (int64_t i = from; i < to; i++) {
        memset(sum, 0, sizeof(double) * (size_t)j->k);
        for (int64_t q = 0; q < j->n; q++) sum[j->code[q]] += sqrt(distance(j->x + i * j->d, j->x + q * j->d, j->d));
        int64_t own = j->code[i];
        if (j->size[own] < 2) {
            j->s[i] = 0;
            continue;
        }
        double a = sum[own] / (j->size[own] - 1), b = INFINITY;
        for (int64_t c = 0; c < j->k; c++)
            if (c != own) b = fmin(b, sum[c] / j->size[c]);
        j->s[i] = (b - a) / fmax(a, b);
    }
    free(sum);
}

/* silhouette_score(X, labels): the mean over samples of (b - a) / max(a, b), a the mean distance to its own
 * cluster, b to the nearest other one. */
FN(f_silhouette_score) {
    (void)argc;
    Data x = NO_DATA, l = NO_DATA;
    LubeValue err;
    if (!xy_in(vm, "silhouette_score", argv[0], argv[1], W_VECTOR, &x, &l, &err)) return err;
    int64_t k;
    double *u = distinct(l.v, l.n, &k);
    LubeValue r;
    if (k < 2 || k > x.n - 1)
        r = lube->error(vm, "silhouette_score(): there are %.0f clusters: it needs 2 to %.0f (the samples less one)",
                        (double)k, (double)(x.n - 1));
    else {
        SilJob j = {x.v, (int64_t *)alloc(sizeof(int64_t) * (size_t)x.n), (double *)alloc(sizeof(double) * (size_t)k),
                    x.n, x.d, k, (double *)alloc(sizeof(double) * (size_t)x.n)};
        for (int64_t i = 0; i < x.n; i++) ((double *)j.size)[((int64_t *)j.code)[i] = find_value(u, k, l.v[i])]++;
        arr->parallel(x.n, (double)x.n * (double)x.n * (double)x.d < 1e6 ? x.n : 64, silhouette_rows, &j);
        double s = 0;
        for (int64_t i = 0; i < x.n; i++) s += j.s[i];
        r = lube->number_value(s / (double)x.n);
        free((void *)j.code);
        free((void *)j.size);
        free(j.s);
    }
    free(u);
    data_free(&x);
    data_free(&l);
    return r;
}

/* adjusted_rand_score(labels_true, labels_pred): how alike two clusterings are, 1 for the same, about 0 by chance. */
FN(f_adjusted_rand_score) {
    (void)argc;
    Data t = NO_DATA, p = NO_DATA;
    LubeValue err;
    if (!pair_in(vm, "adjusted_rand_score", argv, W_VECTOR, &t, &p, &err)) return err;
    int64_t ka, kb, n = t.n;
    double *ua = distinct(t.v, n, &ka), *ub = distinct(p.v, n, &kb);
    double *table = (double *)alloc(sizeof(double) * (size_t)(ka * kb)), *ra = (double *)alloc(sizeof(double) * (size_t)ka);
    double *rb = (double *)alloc(sizeof(double) * (size_t)kb), sum_sq = 0, sa = 0, sb = 0, nn = (double)n;
    for (int64_t i = 0; i < n; i++) {
        int64_t a = find_value(ua, ka, t.v[i]), b = find_value(ub, kb, p.v[i]);
        table[a * kb + b]++;
        ra[a]++;
        rb[b]++;
    }
    for (int64_t i = 0; i < ka * kb; i++) sum_sq += table[i] * table[i];
    for (int64_t i = 0; i < ka; i++) sa += ra[i] * ra[i];
    for (int64_t i = 0; i < kb; i++) sb += rb[i] * rb[i];
    /* the pair confusion matrix (sklearn's) */
    double tp = sum_sq - nn, fp = sb - sum_sq, fn = sa - sum_sq, tn = nn * nn - fp - fn - sum_sq;
    double ari = fn == 0 && fp == 0 ? 1.0 : 2.0 * (tp * tn - fn * fp) / ((tp + fn) * (fn + tn) + (tp + fp) * (fp + tn));
    free(ua);
    free(ub);
    free(table);
    free(ra);
    free(rb);
    data_free(&t);
    data_free(&p);
    return lube->number_value(ari);
}
