"""array.py: numpy (array.lube does the same with Lube's plugin array): making arrays, every elementwise operator
and ufunc, comparisons and masks, where and clip, reductions over everything and along each axis, sorting and
selecting, indexing, slicing, changing items, shapes, dtypes, and files. Both sides draw the same numbers
(RandomState, seeded alike). The size is the number of elements (or of iterations, for the loops).
Lube's Numbers are float64, so its arange and full are too: here they are asked for float64."""
from harness import TMP, bench, bench_chk, bench_tol  # (first: see harness.py)
import math
import os

import numpy as np
from safetensors.numpy import load_file, save_file


try:
    from scipy.special import erf
except ImportError:  # (numpy has no erf; scipy comes with scikit-learn)
    def erf(a):
        raise RuntimeError("erf needs scipy")

rnd = np.random


def cols(n):
    return 40 if n <= 1000 else 400 if n <= 200000 else 4000


def weighted(a):
    a = np.asarray(a)
    return float((a.astype(np.float64) * np.arange(a.size, dtype=np.float64).reshape(a.shape)).sum() / a.size)


def main():
    for n in [1000, 100000, 10000000]:
        rnd.seed(10)
        x = rnd.rand(n) * 2 - 1
        p = rnd.rand(n) + 0.5
        q = rnd.rand(n) + 0.5
        pp = 1 + (rnd.rand(n) - 0.5) * 1e-3
        idx = rnd.randint(0, n, n)
        x10 = x * 10
        p1 = p + 1
        q1 = q - 1
        x32 = x.astype(np.float32)
        p32 = p.astype(np.float32)
        q32 = q.astype(np.float32)
        mask = x > 0
        mask2 = q1 > 0
        mask_all = p > 0
        c = cols(n)
        r = n // c
        m2 = x.reshape(r, c)
        row = x[:c]
        col = x[:r].reshape(r, 1)
        inp = {"x": x, "p": p, "q": q, "x10": x10, "p1": p1, "x32": x32, "p32": p32, "mask": mask}

        # ---------- making arrays ----------
        bench("zeros", n, lambda: np.zeros(n))
        bench("ones", n, lambda: np.ones(n))
        bench("full", n, lambda: np.full(n, 7.0))
        bench("arange", n, lambda: np.arange(n, dtype=np.float64))
        bench("linspace", n, lambda: np.linspace(0, 1, n))
        k = round(math.sqrt(n))
        bench("eye", n, lambda: np.eye(k))
        bench("zeros_like", n, lambda: np.zeros_like(x))
        bench("copy", n, lambda: x.copy())
        rnd.seed(11)
        bench("rand", n, lambda: rnd.rand(n))
        rnd.seed(11)
        bench("randn", n, lambda: rnd.randn(n))
        rnd.seed(11)
        bench("randint", n, lambda: rnd.randint(0, 100, n))
        rnd.seed(11)
        bench("uniform", n, lambda: rnd.uniform(-1, 1, n))
        rnd.seed(11)
        bench("normal", n, lambda: rnd.normal(3, 2, n))
        rnd.seed(11)
        bench_chk("permutation", n, lambda: rnd.permutation(n), weighted)
        lst = x.tolist()
        bench("array_from_list", n, lambda: np.array(lst))
        bench("to_list", n, lambda: x.tolist())
        raw = x.tobytes()
        bench("to_bytes", n, lambda: x.tobytes())
        bench("from_bytes", n, lambda: np.frombuffer(raw, np.float64))

        # ---------- elementwise: one array ----------
        unary = [("negative", "x", np.negative), ("abs", "x", np.abs), ("sqrt", "p", np.sqrt), ("cbrt", "x", np.cbrt),
                 ("exp", "x", np.exp), ("exp2", "x", np.exp2), ("expm1", "x", np.expm1), ("log", "p", np.log),
                 ("log2", "p", np.log2), ("log10", "p", np.log10), ("log1p", "p", np.log1p), ("sin", "x", np.sin),
                 ("cos", "x", np.cos), ("tan", "x", np.tan), ("arcsin", "x", np.arcsin), ("arccos", "x", np.arccos),
                 ("arctan", "x", np.arctan), ("sinh", "x", np.sinh), ("cosh", "x", np.cosh), ("tanh", "x", np.tanh),
                 ("arcsinh", "x", np.arcsinh), ("arccosh", "p1", np.arccosh), ("arctanh", "x", np.arctanh),
                 ("erf", "x", erf), ("floor", "x10", np.floor), ("ceil", "x10", np.ceil), ("trunc", "x10", np.trunc),
                 ("rint", "x10", np.rint), ("round_2", "x10", lambda a: np.round(a, 2)), ("sign", "x", np.sign),
                 ("square", "x", np.square), ("reciprocal", "p", np.reciprocal), ("isnan", "x", np.isnan),
                 ("isinf", "x", np.isinf), ("isfinite", "x", np.isfinite), ("logical_not", "mask", np.logical_not),
                 ("abs_f32", "x32", np.abs), ("sqrt_f32", "p32", np.sqrt), ("exp_f32", "x32", np.exp),
                 ("log_f32", "p32", np.log), ("sin_f32", "x32", np.sin), ("tanh_f32", "x32", np.tanh)]
        for name, key, f in unary:
            a = inp[key]
            bench(name, n, lambda f=f, a=a: f(a))

        # ---------- elementwise: two arrays, or an array and a number ----------
        bench("add", n, lambda: x + q)
        bench("subtract", n, lambda: x - q)
        bench("multiply", n, lambda: x * q)
        bench("divide", n, lambda: x / q)
        bench("mod", n, lambda: x % q)
        bench("power", n, lambda: p ** q)
        bench("add_scalar", n, lambda: x + 1.5)
        bench("multiply_scalar", n, lambda: x * 2.5)
        bench("power_scalar_2", n, lambda: x ** 2)
        bench("maximum", n, lambda: np.maximum(x, q1))
        bench("minimum", n, lambda: np.minimum(x, q1))
        bench("arctan2", n, lambda: np.arctan2(x, q))
        bench("hypot", n, lambda: np.hypot(x, q))
        bench("fmod", n, lambda: np.fmod(x, q))
        bench("floor_divide", n, lambda: np.floor_divide(x10, q))
        bench("copysign", n, lambda: np.copysign(q, x))
        bench("add_f32", n, lambda: x32 + q32)
        bench("multiply_f32", n, lambda: x32 * q32)
        bench("divide_f32", n, lambda: x32 / q32)
        bench("expr_axpy", n, lambda: 2.5 * x + q)
        bench("expr_poly", n, lambda: x * x * 3 + x * 2 + 1)
        bench("broadcast_row", n, lambda: m2 + row)
        bench("broadcast_col", n, lambda: m2 * col)

        # ---------- comparisons and masks, where, clip ----------
        bench("less", n, lambda: x < q1)
        bench("greater_scalar", n, lambda: x > 0)
        bench("equal", n, lambda: x == q1)
        bench("logical_and", n, lambda: np.logical_and(mask, mask2))
        bench("logical_or", n, lambda: np.logical_or(mask, mask2))
        bench("logical_xor", n, lambda: np.logical_xor(mask, mask2))
        bench("isclose", n, lambda: np.isclose(x, q1))
        bench("allclose", n, lambda: np.allclose(x, x))
        bench("bool_index", n, lambda: x[mask])
        bench("where_scalar", n, lambda: np.where(mask, x, 0.0))
        bench("where_arrays", n, lambda: np.where(mask, x, q))
        bench("nonzero", n, lambda: np.nonzero(mask))
        bench("clip", n, lambda: np.clip(x, -0.5, 0.5))

        def setitem_mask():
            b = x.copy()
            b[mask] = 0
            return b
        bench("setitem_mask", n, setitem_mask)

        # ---------- reductions over everything ----------
        bench("sum", n, lambda: x.sum())
        bench("prod", n, lambda: pp.prod())
        bench("mean", n, lambda: x.mean())
        bench("std", n, lambda: x.std())
        bench("var", n, lambda: x.var())
        bench("min", n, lambda: x.min())
        bench("max", n, lambda: x.max())
        bench("argmin", n, lambda: x.argmin())
        bench("argmax", n, lambda: x.argmax())
        bench("ptp", n, lambda: np.ptp(x))
        bench("any", n, lambda: mask.any())
        bench("all", n, lambda: mask_all.all())
        bench("count_nonzero", n, lambda: np.count_nonzero(mask))
        bench("average_weighted", n, lambda: np.average(x, weights=p))
        bench("dot_1d", n, lambda: np.dot(x, q))
        bench("cumsum", n, lambda: x.cumsum())
        bench("cumprod", n, lambda: pp.cumprod())
        bench("sum_f32", n, lambda: x32.sum())
        bench("mean_f32", n, lambda: x32.mean())
        bench("max_f32", n, lambda: x32.max())
        bench("std_f32", n, lambda: x32.std())
        bench("cumsum_f32", n, lambda: x32.cumsum())

        # ---------- reductions along an axis ----------
        for axis in [0, 1]:
            bench(f"sum_axis{axis}", n, lambda: m2.sum(axis))
            bench(f"mean_axis{axis}", n, lambda: m2.mean(axis))
            bench(f"std_axis{axis}", n, lambda: m2.std(axis))
            bench(f"min_axis{axis}", n, lambda: m2.min(axis))
            bench(f"max_axis{axis}", n, lambda: m2.max(axis))
            bench(f"argmax_axis{axis}", n, lambda: m2.argmax(axis))
            bench(f"cumsum_axis{axis}", n, lambda: m2.cumsum(axis))

        # ---------- indexing, slicing, changing items ----------
        bench("slice_view", n, lambda: x[1:n - 1])
        bench("slice_step_copy", n, lambda: x[::2].copy())
        bench("slice_2d_view", n, lambda: m2[1:r - 1, 1:c - 1])
        bench("fancy_index", n, lambda: x[idx])
        rows = idx[:r] % r
        bench("fancy_rows", n, lambda: m2[rows])
        bench("take_3", n, lambda: x[[0, 5, 9]])

        def setitem_slice():
            b = x.copy()
            b[10:n - 10] = 1.5
            return b
        bench("setitem_slice", n, setitem_slice)

        def setitem_fancy():
            b = x.copy()
            b[idx] = 0
            return b
        bench("setitem_fancy", n, setitem_fancy)

        # ---------- shapes and dtypes ----------
        bench("reshape", n, lambda: x.reshape(r, c))
        bench("transpose_view", n, lambda: m2.T)
        bench("transpose_copy", n, lambda: m2.T.copy())
        bench("flatten", n, lambda: m2.flatten())
        bench("ravel_transposed", n, lambda: m2.T.ravel())
        bench("concatenate", n, lambda: np.concatenate([x, q]))
        bench("concatenate_axis1", n, lambda: np.concatenate([m2, m2], 1))
        bench("stack", n, lambda: np.stack([x, q]))
        bench("vstack", n, lambda: np.vstack([m2, m2]))
        bench("hstack", n, lambda: np.hstack([m2, m2]))
        bench("tile", n, lambda: np.tile(x, 2))
        bench("repeat", n, lambda: np.repeat(x, 2))
        bench("roll", n, lambda: np.roll(x, 1))
        bench("flip", n, lambda: np.flip(x))
        bench("astype_f64_f32", n, lambda: x.astype(np.float32))
        bench("astype_f32_f64", n, lambda: x32.astype(np.float64))
        bench("astype_f64_i64", n, lambda: x10.astype(np.int64))
        bench("astype_f64_bool", n, lambda: x.astype(bool))

        # ---------- files (written first, so that each case can run alone) ----------
        npy = f"{TMP}/arr_{n}_py.npy"
        st = f"{TMP}/arr_{n}_py.safetensors"
        np.save(npy, x)
        save_file({"a": x, "b": x32}, st)
        bench_chk("save_npy", n, lambda: np.save(npy, x), lambda r: 0)
        bench("load_npy", n, lambda: np.load(npy))
        bench_chk("save_safetensors", n, lambda: save_file({"a": x, "b": x32}, st), lambda r: 0)
        bench("load_safetensors", n, lambda: load_file(st))
        os.remove(npy)
        os.remove(st)
        if n <= 100000:
            txt = f"{TMP}/arr_{n}_py.txt"
            np.savetxt(txt, x, fmt="%.6f")
            bench_chk("savetxt", n, lambda: np.savetxt(txt, x, fmt="%.6f"), lambda r: 0)
            bench_tol("loadtxt", n, lambda: np.loadtxt(txt), None, 1e-6)
            os.remove(txt)
        del x, p, q, pp, idx, x10, p1, q1, x32, p32, q32, mask, mask2, mask_all, m2, inp, lst, raw

    # ---------- loops over items: scalar indexing and changes ----------
    for n in [1000, 100000, 1000000]:
        rnd.seed(12)
        x = rnd.rand(n)
        g = rnd.rand(1000, 1000)

        def getitem_scalar_loop():
            s = 0
            for i in range(n):
                s += x[i]
            return s
        bench("getitem_scalar_loop", n, getitem_scalar_loop)

        def getitem_2d_loop():
            s = 0
            for i in range(n):
                s += g[i % 1000, (i * 7) % 1000]
            return s
        bench("getitem_2d_loop", n, getitem_2d_loop)

        def setitem_scalar_loop():
            b = np.zeros(n)
            for i in range(n):
                b[i] = i
            return b
        bench("setitem_scalar_loop", n, setitem_scalar_loop)

        def iterate():
            s = 0
            for v in x:
                s += v
            return s
        bench("iterate", n, iterate)

    # ---------- sorting and selecting ----------
    for n in [1000, 200000, 10000000]:
        rnd.seed(13)
        s = rnd.rand(n)
        si = rnd.randint(0, n, n)
        few = rnd.randint(0, max(1, n // 10), n)
        needles = rnd.rand(min(n, 1000000))
        bins = rnd.randint(0, 1000, n)
        s32 = s.astype(np.float32)
        s2 = s.reshape(n // 100, 100)
        hay = np.sort(s)
        bench_chk("sort", n, lambda: np.sort(s), weighted)
        bench_chk("sort_f32", n, lambda: np.sort(s32), weighted)
        bench_chk("sort_int64", n, lambda: np.sort(si), weighted)
        bench_chk("argsort", n, lambda: np.argsort(s), lambda r: weighted(s[r]))
        bench_chk("argsort_int64", n, lambda: np.argsort(si), lambda r: weighted(si[r]))
        bench_chk("sort_axis1", n, lambda: np.sort(s2, 1), weighted)
        bench("median", n, lambda: np.median(s))
        bench("median_axis1", n, lambda: np.median(s2, 1))
        bench("percentile", n, lambda: np.percentile(s, [5, 50, 95]))
        bench("quantile", n, lambda: np.quantile(s, 0.3))
        bench("unique_ints", n, lambda: np.unique(few))
        bench("unique_floats", n, lambda: np.unique(s))
        bench("unique_counts", n, lambda: np.unique(few, return_counts=True))
        bench("searchsorted", n, lambda: np.searchsorted(hay, needles))
        bench("bincount", n, lambda: np.bincount(bins))
        bench("partition", n, lambda: np.partition(s, n // 2))


if __name__ == "__main__":
    main()
