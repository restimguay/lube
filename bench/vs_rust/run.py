#!/usr/bin/env python3
"""Lube against Rust: the same programs in both, timed side by side.

    python3 bench/vs_rust/run.py [--runs N] [--lube PATH] [NAME ...]

Each NAME.lube here has a NAME.rs that does the same work and prints the
same output. Rust programs are built with `rustc -C opt-level=3` (a release
build). For each pair it reports the best CPU time (user + system) of N runs, the
peak memory (maximum resident set size), whether the outputs match, and how
many times slower (>1) or faster (<1) Lube is. Then it compares executable
sizes: a program built with `lube --build` against the Rust binary.
"""
import argparse
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "vs_rust")


def run(cmd):
    """Runs cmd; returns (CPU seconds, peak RSS in bytes, stdout, exit code).
    CPU time (user + system) is steadier than wall-clock time on a busy machine."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.read()
    _, status, usage = os.wait4(p.pid, 0)
    elapsed = usage.ru_utime + usage.ru_stime
    p.stdout.close()
    rss = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024  # macOS: bytes; Linux: KB
    return elapsed, rss, out, os.waitstatus_to_exitcode(status)


def best(cmd, runs):
    times, rss, out, code = [], 0, b"", 0
    for _ in range(runs):
        t, r, out, code = run(cmd)
        times.append(t)
        rss = max(rss, r)
        if code:
            break
    return min(times), rss, out, code


def fmt_time(t):
    return f"{t * 1000:8.1f} ms" if t < 1 else f"{t:8.2f} s "


def fmt_mem(b):
    return f"{b / 1048576:6.1f} MB"


def counted(cmd):
    """macOS: (cycles, instructions) for cmd, which other processes barely affect."""
    p = subprocess.run(["/usr/bin/time", "-l"] + cmd, capture_output=True, text=True)
    found = {}
    for line in p.stderr.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1] in ("instructions retired", "cycles elapsed"):
            found[parts[1]] = int(parts[0])
    return found.get("cycles elapsed", 0), found.get("instructions retired", 0)


def compare(args, names):
    """--base: each benchmark with both lubes, alternately; the best of each. On
    macOS this counts CPU cycles (and instructions), elsewhere CPU time."""
    mac = sys.platform == "darwin"
    unit = "Gcycles" if mac else "CPU time"
    print(f"{'benchmark':10} {'base ' + unit:>14} {'new':>9} {'speedup':>8}" + ("   instructions base -> new" if mac else ""))
    gains = []
    for name in names:
        prog = os.path.join(HERE, name + ".lube")
        b = n = (float("inf"), 0)
        for _ in range(args.runs):
            b = min(b, counted([args.base, prog]) if mac else (run([args.base, prog])[0], 0))
            n = min(n, counted([args.lube, prog]) if mac else (run([args.lube, prog])[0], 0))
        gains.append(b[0] / n[0])
        shown = (lambda c: f"{c / 1e9:9.3f}") if mac else (lambda t: fmt_time(t))
        extra = f"   {b[1] / 1e9:6.2f}G -> {n[1] / 1e9:6.2f}G" if mac else ""
        print(f"{name:10} {shown(b[0]):>14} {shown(n[0]):>9} {b[0] / n[0]:7.2f}x{extra}")
    print(f"{'geo. mean':10} {'':14} {'':9} {math.exp(sum(map(math.log, gains)) / len(gains)):7.2f}x")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=3, help="runs of each program (the best counts)")
    ap.add_argument("--lube", default=os.path.join(ROOT, "lube"), help="the lube to test")
    ap.add_argument("--base", help="another lube to compare with (runs alternate, so both see the same load)")
    ap.add_argument("names", nargs="*", help="benchmarks to run (default: all)")
    args = ap.parse_args()
    names = args.names or sorted(f[:-5] for f in os.listdir(HERE) if f.endswith(".lube"))
    os.makedirs(OUT, exist_ok=True)
    if args.base:
        compare(args, names)
        return

    print(f"{'benchmark':10} {'Lube':>11} {'Rust':>11} {'Lube/Rust':>10}   {'Lube mem':>9} {'Rust mem':>9}  output")
    ratios = []
    for name in names:
        rs, bin_ = os.path.join(HERE, name + ".rs"), os.path.join(OUT, name)
        if not os.path.exists(bin_) or os.path.getmtime(bin_) < os.path.getmtime(rs):
            subprocess.run(["rustc", "-C", "opt-level=3", "-o", bin_, rs], check=True)
        lt, lm, lout, lcode = best([args.lube, os.path.join(HERE, name + ".lube")], args.runs)
        rt, rm, rout, _ = best([bin_], args.runs)
        same = "same" if lout == rout and lcode == 0 else "DIFFERENT"
        ratio = lt / rt
        ratios.append(ratio)
        print(f"{name:10} {fmt_time(lt)} {fmt_time(rt)} {ratio:9.1f}x   {fmt_mem(lm)} {fmt_mem(rm)}  {same}")
        if same != "same":
            print("    lube:", lout.decode(errors="replace").strip()[:300])
            print("    rust:", rout.decode(errors="replace").strip()[:300])
    gm = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print(f"{'geo. mean':10} {'':11} {'':11} {gm:9.1f}x")

    hello = os.path.join(OUT, "hello_lube")
    subprocess.run([args.lube, "--build", os.path.join(HERE, "hello.lube"), "-o", hello], check=True,
                   stdout=subprocess.DEVNULL)
    rust_hello = os.path.join(OUT, "hello")
    if not os.path.exists(rust_hello):
        subprocess.run(["rustc", "-C", "opt-level=3", "-o", rust_hello, os.path.join(HERE, "hello.rs")], check=True)
    stripped = rust_hello + "_stripped"
    subprocess.run(["rustc", "-C", "opt-level=3", "-C", "strip=symbols", "-o", stripped,
                    os.path.join(HERE, "hello.rs")], check=True)
    print(f"\nexecutable size (hello): lube --build {os.path.getsize(hello) / 1024:.0f} KB, "
          f"Rust {os.path.getsize(rust_hello) / 1024:.0f} KB (stripped: {os.path.getsize(stripped) / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
