"""learn.py: scikit-learn (learn.lube does the same with Lube's plugin learn): fit and predict or transform for each
model learn has, model selection and metrics, on make_blobs' and make_regression's data (the same numbers on both
sides); the size is samples x features. Forests use every core (n_jobs=-1), as Lube's do; the rest keep sklearn's
defaults. Convergence warnings are silenced."""
from harness import bench, bench_chk, bench_tol  # (first: see harness.py)
import math
import warnings

import numpy as np
from sklearn.cluster import DBSCAN, KMeans
from sklearn.datasets import make_blobs, make_regression
from sklearn.decomposition import PCA
from sklearn.ensemble import RandomForestClassifier, RandomForestRegressor
from sklearn.linear_model import ElasticNet, Lasso, LinearRegression, LogisticRegression, Ridge
from sklearn.metrics import (accuracy_score, adjusted_rand_score, classification_report, confusion_matrix, f1_score,
                             log_loss, mean_squared_error, precision_score, r2_score, recall_score, roc_auc_score,
                             silhouette_score)
from sklearn.model_selection import KFold, cross_val_score, train_test_split
from sklearn.naive_bayes import GaussianNB
from sklearn.neighbors import KNeighborsClassifier, KNeighborsRegressor
from sklearn.preprocessing import LabelEncoder, MinMaxScaler, OneHotEncoder, PolynomialFeatures, StandardScaler
from sklearn.svm import LinearSVC
from sklearn.tree import DecisionTreeClassifier, DecisionTreeRegressor


warnings.filterwarnings("ignore")
rnd = np.random


def score_on(X, y):
    return lambda m: m.score(X, y)


def absum(a):
    return float(np.abs(a).sum())


def main():
    for n, f in [(1000, 10), (20000, 20)]:
        size = f"{n}x{f}"
        X, y = make_blobs(n_samples=n, n_features=f, centers=3, cluster_std=4.0, random_state=0)
        yb = (y == 0).astype(np.int64)
        Xr, yr = make_regression(n_samples=n, n_features=f, noise=10, random_state=0)
        Xs = StandardScaler().fit_transform(X)
        Xq = X[:1000]
        nd = min(n, 5000)
        Xd = Xs[:nd]
        rnd.seed(40)
        cat = rnd.randint(0, 5, (n, 4))

        # ---------- linear models ----------
        bench_chk("LinearRegression.fit", size, lambda: LinearRegression().fit(Xr, yr), score_on(Xr, yr))
        linr = LinearRegression().fit(Xr, yr)
        bench("LinearRegression.predict", size, lambda: linr.predict(Xr))
        bench_chk("Ridge.fit", size, lambda: Ridge(alpha=1.0).fit(Xr, yr), score_on(Xr, yr))
        bench_tol("Lasso.fit", size, lambda: Lasso(alpha=1.0).fit(Xr, yr), score_on(Xr, yr), 1e-2)
        bench_tol("ElasticNet.fit", size, lambda: ElasticNet(alpha=1.0, l1_ratio=0.5).fit(Xr, yr), score_on(Xr, yr), 1e-2)
        bench_tol("LogisticRegression.fit_multinomial", size, lambda: LogisticRegression(max_iter=200).fit(Xs, y),
                  score_on(Xs, y), 1e-2)
        bench_tol("LogisticRegression.fit_binary", size, lambda: LogisticRegression(max_iter=200).fit(Xs, yb),
                  score_on(Xs, yb), 1e-2)
        logit = LogisticRegression(max_iter=200).fit(Xs, y)
        bench_tol("LogisticRegression.predict_proba", size, lambda: logit.predict_proba(Xs), None, 1e-2)
        bench_tol("LinearSVC.fit", size, lambda: LinearSVC().fit(Xs, yb), score_on(Xs, yb), 1e-2)

        # ---------- neighbors ----------
        knn = KNeighborsClassifier(5).fit(X, y)
        bench_chk("KNeighborsClassifier.fit", size, lambda: KNeighborsClassifier(5).fit(X, y), lambda m: m.n_features_in_)
        bench("KNeighborsClassifier.predict_1000", size, lambda: knn.predict(Xq))
        knr = KNeighborsRegressor(5).fit(Xr, yr)
        bench("KNeighborsRegressor.predict_1000", size, lambda: knr.predict(Xr[:1000]))

        # ---------- trees and forests ----------
        bench_chk("DecisionTreeClassifier.fit", size, lambda: DecisionTreeClassifier(random_state=0).fit(X, y),
                  score_on(X, y))
        bench_chk("DecisionTreeRegressor.fit", size, lambda: DecisionTreeRegressor(max_depth=10, random_state=0).fit(Xr, yr),
                  score_on(Xr, yr))
        bench_tol("RandomForestClassifier.fit", size,
                  lambda: RandomForestClassifier(n_estimators=50, random_state=0, n_jobs=-1).fit(X, y), score_on(X, y), 0.05)
        bench_tol("RandomForestRegressor.fit", size,
                  lambda: RandomForestRegressor(n_estimators=50, max_depth=10, random_state=0, n_jobs=-1).fit(Xr, yr),
                  score_on(Xr, yr), 0.05)
        forest = RandomForestClassifier(n_estimators=50, random_state=0, n_jobs=-1).fit(X, y)
        bench_tol("RandomForestClassifier.predict", size, lambda: forest.predict(X), None, 0.05)

        # ---------- the others ----------
        bench_chk("GaussianNB.fit", size, lambda: GaussianNB().fit(X, y), score_on(X, y))
        nb = GaussianNB().fit(X, y)
        bench("GaussianNB.predict", size, lambda: nb.predict(X))
        bench_tol("KMeans.fit", size, lambda: KMeans(n_clusters=3, random_state=0).fit(X), lambda m: m.inertia_, 1e-2)
        km = KMeans(n_clusters=3, random_state=0).fit(X)
        bench("KMeans.predict", size, lambda: km.predict(X))
        eps = math.sqrt(f)
        bench("DBSCAN.fit_predict", f"{nd}x{f}", lambda: DBSCAN(eps=eps, min_samples=5).fit_predict(Xd))
        bench_chk("PCA.fit", size, lambda: PCA(5).fit(X), lambda m: m.explained_variance_ratio_)
        pca = PCA(5).fit(X)
        bench_chk("PCA.transform", size, lambda: pca.transform(X), absum)
        bench("StandardScaler.fit_transform", size, lambda: StandardScaler().fit_transform(X))
        bench("MinMaxScaler.fit_transform", size, lambda: MinMaxScaler().fit_transform(X))
        bench("LabelEncoder.fit_transform", size, lambda: LabelEncoder().fit_transform(y))
        bench("OneHotEncoder.fit_transform", size, lambda: OneHotEncoder(sparse_output=False).fit_transform(cat))
        bench("PolynomialFeatures.fit_transform", size, lambda: PolynomialFeatures(2).fit_transform(X[:, :5]))

        # ---------- choosing models ----------
        bench("train_test_split", size, lambda: train_test_split(X, y, test_size=0.25, random_state=0))
        bench_chk("KFold.split", size, lambda: list(KFold(n_splits=5, shuffle=True, random_state=0).split(X)),
                  lambda r: [s[1] for s in r])
        bench("cross_val_score_GaussianNB", size, lambda: cross_val_score(GaussianNB(), X, y, cv=5))
        bench("cross_val_score_Ridge", size, lambda: cross_val_score(Ridge(alpha=1.0), Xr, yr, cv=5))

        # ---------- metrics ----------
        pred = nb.predict(X)
        proba = nb.predict_proba(X)
        rpred = linr.predict(Xr)
        bench("accuracy_score", size, lambda: accuracy_score(y, pred))
        bench("precision_score_macro", size, lambda: precision_score(y, pred, average="macro"))
        bench("recall_score_macro", size, lambda: recall_score(y, pred, average="macro"))
        bench("f1_score_macro", size, lambda: f1_score(y, pred, average="macro"))
        bench("confusion_matrix", size, lambda: confusion_matrix(y, pred))
        bench("classification_report", size, lambda: classification_report(y, pred))
        bench("log_loss", size, lambda: log_loss(y, proba))
        bench("roc_auc_score", size, lambda: roc_auc_score(yb, proba[:, 0]))
        bench("mean_squared_error", size, lambda: mean_squared_error(yr, rpred))
        bench("r2_score", size, lambda: r2_score(yr, rpred))
        bench("adjusted_rand_score", size, lambda: adjusted_rand_score(y, km.labels_))
        bench("silhouette_score", f"{nd}x{f}", lambda: silhouette_score(X[:nd], y[:nd]))


if __name__ == "__main__":
    main()
