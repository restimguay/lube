/*
 * learn.c - the plugin learn: machine learning on Arrays, as scikit-learn's.
 *
 *   import learn                       (with import array as np for the Arrays)
 *   let m = learn.Ridge({alpha: 0.5})  a model: options as an object, the names sklearn has
 *   m.fit(X, y)                        X an Array (or nested lists) of a row per sample; returns m
 *   m.predict(X), m.score(X, y)        and predict_proba, decision_function, transform...
 *   m.coef_, m.classes_                what fit found, as properties
 *
 *   linear       LinearRegression, Ridge, Lasso, ElasticNet, LogisticRegression, LinearSVC
 *   neighbors    KNeighborsClassifier, KNeighborsRegressor
 *   trees        DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
 *                RandomForestRegressor (trees on several threads)
 *   others       GaussianNB, KMeans, DBSCAN, PCA
 *   preparing    StandardScaler, MinMaxScaler, LabelEncoder, OneHotEncoder, PolynomialFeatures
 *   choosing     train_test_split, KFold, StratifiedKFold, cross_val_score
 *   metrics      accuracy_score, precision_score, recall_score, f1_score, confusion_matrix,
 *                classification_report, mean_squared_error, mean_absolute_error, r2_score,
 *                log_loss, roc_auc_score, silhouette_score...
 *   datasets     load_iris, make_blobs, make_classification, make_regression, make_moons
 *
 * Models work in float64 C loops over the Arrays' memory; big products go
 * through array's matmul (the system's BLAS), and big loops through its
 * threads, in blocks whose sums never depend on how many threads there
 * are. Each call that draws random numbers has a generator of its own,
 * numpy's: random_state seeds it (giving RandomState's numbers, so sklearn's
 * splits and data), and without one array's generator does (np.seed).
 */
#include "array.h"

#ifdef __clang__  /* (no fused multiply-adds: the same results on every system; gcc's -std=c99 makes none) */
#pragma STDC FP_CONTRACT OFF
#endif

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;
static const ArrayExports *arr;

#include "learn/core.c"
#include "learn/linear.c"
#include "learn/neighbors.c"
#include "learn/tree.c"
#include "learn/cluster.c"
#include "learn/transform.c"
#include "learn/models.c"
#include "learn/metrics.c"
#include "learn/selection.c"
#include "learn/datasets.c"

LUBE_PLUGIN(learn) {
    lube = api;
    if (api->version < 2) return -1;
    arr = (const ArrayExports *)api->require(plugin, "array");
    if (!arr || arr->version < ARRAY_EXPORTS) return -1;
    declare_models(plugin);
    struct { const char *name; LubeFn fn; int min, max; } fns[] = {
        {"train_test_split", f_train_test_split, 1, -1}, {"cross_val_score", f_cross_val_score, 3, 4},
        {"accuracy_score", f_accuracy_score, 2, 3}, {"precision_score", f_precision_score, 2, 3},
        {"recall_score", f_recall_score, 2, 3}, {"f1_score", f_f1_score, 2, 3}, {"fbeta_score", f_fbeta_score, 3, 4},
        {"confusion_matrix", f_confusion_matrix, 2, 3}, {"classification_report", f_classification_report, 2, 3},
        {"balanced_accuracy_score", f_balanced_accuracy_score, 2, 2},
        {"mean_squared_error", f_mean_squared_error, 2, 2}, {"root_mean_squared_error", f_root_mean_squared_error, 2, 2},
        {"mean_absolute_error", f_mean_absolute_error, 2, 2}, {"r2_score", f_r2_score, 2, 2},
        {"explained_variance_score", f_explained_variance_score, 2, 2}, {"log_loss", f_log_loss, 2, 3},
        {"roc_auc_score", f_roc_auc_score, 2, 3}, {"silhouette_score", f_silhouette_score, 2, 2},
        {"adjusted_rand_score", f_adjusted_rand_score, 2, 2},
        {"load_iris", f_load_iris, 0, 1}, {"make_blobs", f_make_blobs, 0, 1},
        {"make_classification", f_make_classification, 0, 1}, {"make_regression", f_make_regression, 0, 1},
        {"make_moons", f_make_moons, 0, 1},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) api->function(plugin, fns[i].name, fns[i].fn, fns[i].min, fns[i].max);
    return LUBE_PLUGIN_API;
}
