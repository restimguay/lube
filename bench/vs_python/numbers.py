"""numbers.py: Python's math, cmath, statistics and random (numbers.lube does the same with Lube's plugins of those
names). The size is the number of values (a loop over them, or a list of them)."""
from harness import bench, bench_chk  # (first: see harness.py)
import cmath
import math
import random
import statistics



def main():
    for n in [1000, 100000]:
        random.seed(7)
        xs = [random.uniform(0.1, 10) for i in range(n)]
        ys = [random.uniform(0.1, 10) for i in range(n)]
        near1 = [1 + (x - 5) / 1e5 for x in xs]
        ints = [random.randint(1, 1000000) for i in range(n)]

        # ---------- math: a loop calling one function, summing its results ----------
        unary = [("sqrt", math.sqrt), ("exp", math.exp), ("log", math.log), ("log10", math.log10), ("sin", math.sin),
                 ("tanh", math.tanh), ("cbrt", math.cbrt), ("lgamma", math.lgamma), ("gamma", math.gamma),
                 ("erf", math.erf), ("floor", math.floor), ("degrees", math.degrees)]
        for name, f in unary:
            def loop(f=f):
                s = 0
                for x in xs:
                    s += f(x)
                return s
            bench("math." + name, n, loop)

        def atan2():
            s = 0
            for i in range(n):
                s += math.atan2(xs[i], ys[i])
            return s
        bench("math.atan2", n, atan2)

        def hypot():
            s = 0
            for i in range(n):
                s += math.hypot(xs[i], ys[i])
            return s
        bench("math.hypot", n, hypot)

        def pow_():
            s = 0
            for x in xs:
                s += math.pow(x, 1.5)
            return s
        bench("math.pow", n, pow_)

        def fmod():
            s = 0
            for x in xs:
                s += math.fmod(x, 3)
            return s
        bench("math.fmod", n, fmod)

        def isclose():
            c = 0
            for i in range(n):
                if math.isclose(xs[i], ys[i], rel_tol=0.1):
                    c += 1
            return c
        bench("math.isclose", n, isclose)

        def gcd():
            s = 0
            for i in range(n - 1):
                s += math.gcd(ints[i], ints[i + 1])
            return s
        bench("math.gcd", n, gcd)

        def isqrt():
            s = 0
            for k in ints:
                s += math.isqrt(k)
            return s
        bench("math.isqrt", n, isqrt)
        bench("math.fsum", n, lambda: math.fsum(xs))
        bench("math.prod", n, lambda: math.prod(near1))
        bench("math.sumprod", n, lambda: math.sumprod(xs, ys))
        bench("math.dist", n, lambda: math.dist(xs, ys))
        bench("builtin_sum", n, lambda: sum(xs))

        # ---------- cmath ----------
        zs = [complex(xs[i] - 5, ys[i] - 5) for i in range(n)]
        cunary = [("exp", lambda z: cmath.exp(z / 5)), ("log", cmath.log), ("sqrt", cmath.sqrt), ("sin", cmath.sin),
                  ("atanh", lambda z: cmath.atanh(z / 10))]
        for name, f in cunary:
            def cloop(f=f):
                s = complex(0, 0)
                for z in zs:
                    s = s + f(z)
                return s
            bench("cmath." + name, n, cloop)

        def phase():
            s = 0
            for z in zs:
                s += cmath.phase(z)
            return s
        bench("cmath.phase", n, phase)

        def cabs():
            s = 0
            for z in zs:
                s += abs(z)
            return s
        bench("cmath.abs", n, cabs)

        def polar():
            s = 0
            for z in zs:
                s += cmath.polar(z)[1]
            return s
        bench("cmath.polar", n, polar)

        def complex_arithmetic():
            s = complex(0, 0)
            for i in range(n - 1):
                s = s + zs[i] * zs[i + 1] - zs[i] / (zs[i + 1] + 20)
            return s
        bench("complex_arithmetic", n, complex_arithmetic)

        def complex_make():
            s = 0
            for i in range(n):
                s += complex(xs[i], ys[i]).imag
            return s
        bench("complex_make", n, complex_make)

        # ---------- statistics ----------
        random.seed(8)
        data = [random.gauss(10, 3) for i in range(n)]
        data2 = [x * 2 + random.random() for x in data]
        small_ints = [k % 50 for k in ints]
        bench("statistics.mean", n, lambda: statistics.mean(data))
        bench("statistics.fmean", n, lambda: statistics.fmean(data))
        bench("statistics.geometric_mean", n, lambda: statistics.geometric_mean(xs))
        bench("statistics.harmonic_mean", n, lambda: statistics.harmonic_mean(xs))
        bench("statistics.median", n, lambda: statistics.median(data))
        bench("statistics.mode", n, lambda: statistics.mode(small_ints))
        bench("statistics.multimode", n, lambda: statistics.multimode(small_ints))
        bench("statistics.variance", n, lambda: statistics.variance(data))
        bench("statistics.stdev", n, lambda: statistics.stdev(data))
        bench("statistics.pstdev", n, lambda: statistics.pstdev(data))
        bench("statistics.quantiles", n, lambda: statistics.quantiles(data, n=10))
        bench("statistics.covariance", n, lambda: statistics.covariance(data, data2))
        bench("statistics.correlation", n, lambda: statistics.correlation(data, data2))
        bench("statistics.correlation_ranked", n, lambda: statistics.correlation(data, data2, method="ranked"))
        bench_chk("statistics.linear_regression", n, lambda: statistics.linear_regression(data, data2),
                  lambda r: [r.slope, r.intercept])
        bench_chk("NormalDist.from_samples", n, lambda: statistics.NormalDist.from_samples(data),
                  lambda d: [d.mean, d.stdev])
        nd = statistics.NormalDist(10, 3)

        def cdf():
            s = 0
            for x in xs:
                s += nd.cdf(x)
            return s
        bench("NormalDist.cdf", n, cdf)

        def inv_cdf():
            s = 0
            for x in xs:
                s += nd.inv_cdf(x / 10.5)
            return s
        bench("NormalDist.inv_cdf", n, inv_cdf)
        k = statistics.kde(data, 0.5)

        def kde10():
            s = 0
            for i in range(10):
                s += k(i * 2)
            return s
        bench("kde.pdf_10_points", n, kde10)

        # ---------- random: each case seeded first ----------
        def rloop(draw):
            def loop():
                s = 0
                for i in range(n):
                    s += draw()
                return s
            return loop
        for name, draw in [("random", random.random), ("uniform", lambda: random.uniform(1, 2)),
                           ("randint", lambda: random.randint(1, 100)),
                           ("randrange", lambda: random.randrange(0, 1000, 7)), ("gauss", lambda: random.gauss(0, 1)),
                           ("expovariate", lambda: random.expovariate(2)), ("choice", lambda: random.choice(ints)),
                           ("getrandbits", lambda: random.getrandbits(32))]:
            random.seed(9)
            bench("random." + name, n, rloop(draw))

        def shuffled():
            out = ints[:]
            random.shuffle(out)
            return out
        random.seed(9)
        bench_chk("random.shuffle", n, shuffled, lambda r: r[0] + r[n - 1] * 2 + r[n // 2] * 3)
        random.seed(9)
        bench("random.sample", n, lambda: random.sample(ints, n // 10))
        random.seed(9)
        bench("random.choices", n, lambda: random.choices(ints, k=n))
        random.seed(9)
        bench("random.choices_weighted", n, lambda: random.choices(small_ints, weights=xs, k=n))

    for k in [10, 1000]:
        def seeds(k=k):
            for i in range(k):
                random.seed(i)
            return random.random()
        bench("random.seed", k, seeds)

    for k in [20, 100, 170]:
        bench("math.factorial", k, lambda: math.factorial(k))
    for k in [100, 1000]:
        bench("math.comb", k, lambda: math.comb(k, k // 2))
        bench("math.perm", k, lambda: math.perm(k, 20))


if __name__ == "__main__":
    main()
