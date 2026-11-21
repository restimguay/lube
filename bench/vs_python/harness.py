"""harness.py: the timing and checksums shared by the Python side of bench/vs_python (harness.lube is its twin,
and does the same thing the same way). Each area's script imports it and calls bench() once per case:

    python AREA.py TMPDIR [MIN_SECONDS] [REPEATS] [FILTER] [extra...]

One line per case: case <TAB> size <TAB> seconds per call <TAB> checksum. A case is called once (warm-up, and
its checksum), then calibrated so that one measurement (n calls) takes at least MIN_SECONDS, then measured
REPEATS times (3 when a call takes over 1 s, 1 over 5 s); the best measurement over n is printed.
The checksum is "sum,sum of |x|" of the result (or of what chk() makes of it), and optionally ",rtol".
The garbage collector stays on, as in a real program.
"""
import os
import sys

# This folder goes last: its array.py and numbers.py must not hide the modules of those names. (Each script imports
# harness before anything else.)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path = [p for p in sys.path if os.path.abspath(p) != HERE] + [HERE]

import math  # noqa: E402
import time  # noqa: E402
from decimal import Decimal  # noqa: E402

argv = sys.argv
TMP = argv[1] if len(argv) > 1 else "/tmp"
MIN = float(argv[2]) if len(argv) > 2 else 0.05
REPEATS = int(argv[3]) if len(argv) > 3 else 5
FILTER = argv[4] if len(argv) > 4 else ""
EXTRA = argv[5:]
now = time.perf_counter


def parts(r):
    """[sum, sum of |x|]"""
    if isinstance(r, bool):
        return [1.0, 1.0] if r else [0.0, 0.0]
    if isinstance(r, (int, float)):
        return [float(r), abs(float(r))]
    if isinstance(r, str):
        return [float(len(r)), float(len(r))]
    if isinstance(r, bytes):
        return [float(len(r)), float(len(r))]
    if isinstance(r, Decimal):  # money: in cents, as Lube's units
        u = float(r * 100)
        return [u, abs(u)]
    if isinstance(r, complex):
        return [r.real + r.imag, abs(r.real) + abs(r.imag)]
    mod = type(r).__module__
    if mod == "numpy":
        import numpy as np
        f = np.asarray(r, dtype=np.float64)
        s = float(f.sum())
        return [s, float(np.abs(f).sum())]
    if mod == "torch":
        return parts(r.detach().cpu().numpy())
    if isinstance(r, dict):
        r = list(r.values())
    if isinstance(r, (list, tuple)):
        s = a = 0.0
        for x in r:
            p = parts(x)
            s += p[0]
            a += p[1]
        return [s, a]
    return [float(r), abs(float(r))]


def checksum(r, rtol=0):
    p = parts(r)
    s = ",".join(repr(float(x)) for x in p)
    return f"{s},{rtol}" if rtol else s


def _time(f, n):
    t0 = now()
    for _ in range(n):
        f()
    return now() - t0


def wanted(name):
    """FILTER: a part of the case names to run, or "=" and a list of exact names (=sum,sort)."""
    if not FILTER:
        return True
    if FILTER.startswith("="):
        return name in FILTER[1:].split(",")
    return FILTER in name


def bench_full(name, size, f, chk=None, rtol=0):
    """Times f(); prints its line; returns the seconds per call (or -1 if it failed or was filtered out)."""
    if not wanted(name):
        return -1
    try:
        t0 = now()
        r = f()
        first = now() - t0
        c = "-" if chk == "-" else checksum(r if chk is None else chk(r), rtol)
        n, t = 1, first
        while t < MIN:
            per = max(t / n, 1e-9)
            nxt = math.ceil(MIN * 1.25 / per)
            nxt = min(nxt, n * 100)
            if nxt <= n:
                nxt = n * 2
            n = nxt
            t = _time(f, n)
        reps = REPEATS
        if t / n > 1:
            reps = min(reps, 3)
        if t / n > 5:
            reps = 1
        best = min(_time(f, n) for _ in range(reps))
        print(f"{name}\t{size}\t{best / n!r}\t{c}", flush=True)
        return best / n
    except Exception as e:  # noqa: BLE001 (reported, and the next case runs)
        msg = f"{type(e).__name__}: {e}".replace("\t", " ").replace("\n", " ")
        print(f"{name}\t{size}\terror\t{msg}", flush=True)
        return -1


def bench(name, size, f):
    return bench_full(name, size, f)


def bench_chk(name, size, f, chk):
    return bench_full(name, size, f, chk)


def bench_tol(name, size, f, chk, rtol):
    return bench_full(name, size, f, chk, rtol)


def bench_nochk(name, size, f):
    return bench_full(name, size, f, "-")


def bench_skip(name, size, why):
    if wanted(name):
        print(f"{name}\t{size}\tskip\t{why}", flush=True)
