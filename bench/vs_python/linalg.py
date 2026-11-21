"""linalg.py: numpy's linear algebra (linalg.lube does the same with Lube's plugin array): matrix products in
float64 and float32, solve, inv, det, eigh, eigvalsh, svd, qr, cholesky, lstsq, pinv, norm, matrix_rank, on
n x n matrices. A case whose time at 300 promises more than 10 s at 1000 (n^3.5) is skipped there, with the estimate."""
from harness import bench, bench_chk, bench_skip  # (first: see harness.py)
import numpy as np


rnd = np.random
la = np.linalg
at300 = {}


def absum(a):
    return float(np.abs(a).sum())


def run(name, n, f, chk=None):
    if n == 1000 and name in at300:
        est = at300[name] * (1000 / 300) ** 3.5  # (cubic, and worse: caches)
        if est > 10:
            bench_skip(name, n, f"est. {est:.1f} s a call (from n=300)")
            return
    t = bench(name, n, f) if chk is None else bench_chk(name, n, f, chk)
    if n == 300 and t > 0:
        at300[name] = t


def main():
    for n in [10, 50, 300, 1000]:
        rnd.seed(20)
        A = rnd.rand(n, n)
        A2 = rnd.rand(n, n)
        v = rnd.rand(n)
        B = np.eye(n) + (rnd.rand(n, n) - 0.5) / n
        S = A @ A.T + np.eye(n) * n
        T = rnd.rand(2 * n, n)
        tb = rnd.rand(2 * n)
        A32 = A.astype(np.float32)
        A232 = A2.astype(np.float32)

        run("matmul_f64", n, lambda: A @ A2)
        run("matmul_f32", n, lambda: A32 @ A232)
        run("matvec_f64", n, lambda: A @ v)
        run("outer", n, lambda: np.outer(v, v))
        run("solve", n, lambda: la.solve(B, v))
        run("solve_matrix", n, lambda: la.solve(B, A2))
        run("inv", n, lambda: la.inv(B))
        run("det", n, lambda: la.det(B))
        run("eigh", n, lambda: la.eigh(S), lambda r: [r[0].sum(), absum(r[1])])
        run("eigvalsh", n, lambda: la.eigvalsh(S))
        run("svd", n, lambda: la.svd(A), lambda r: [r[1].sum(), absum(r[0]), absum(r[2])])
        run("svd_values", n, lambda: la.svd(A, compute_uv=False))
        run("qr", n, lambda: la.qr(A), lambda r: [absum(r[0]), absum(r[1])])
        run("cholesky", n, lambda: la.cholesky(S))
        run("lstsq", n, lambda: la.lstsq(T, tb, rcond=None), lambda r: r[0])
        run("pinv", n, lambda: la.pinv(A))
        run("norm_fro", n, lambda: la.norm(A))
        run("norm_2", n, lambda: la.norm(A, 2))
        run("matrix_rank", n, lambda: la.matrix_rank(A))


if __name__ == "__main__":
    main()
