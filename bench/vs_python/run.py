#!/usr/bin/env python3
"""Lube against Python and its C libraries: the same cases in both, timed side by side.

    python3 bench/vs_python/run.py [AREA ...] [--lube PATH] [--python PATH] [--json FILE] [--md FILE]
                                   [--compare OLD.json] [--losers OLD.json] [--filter TEXT] [--model GGUF ...]

Each AREA.lube here has an AREA.py that does the same computation, case by case, at several sizes, and prints
a line per case: case, size, seconds per call (the best of 5 calibrated measurements) and a checksum of the
result. This joins the two and prints a table: the time of each, Python/Lube (above 1: Lube is faster), and
whether the checksums agree. Areas: core, money, numbers, array, linalg, nn, learn, llm, isolates.

--json and --md write the results; --compare OLD.json shows each case's Lube time before and after;
--losers OLD.json runs again only the cases Python won in OLD.json (and those whose checksums differed), and
keeps the better time of the two runs for each side (with --json, the merged results). --model gives the llm area its GGUF files (or $BENCH_MODELS,
separated by ':'); without one it uses the tiny test model in tests/models.
"""
import argparse
import datetime
import json
import math
import os
import platform
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
AREAS = ["core", "money", "numbers", "array", "linalg", "nn", "learn", "llm", "isolates"]
RTOL = 1e-4  # checksums: |sum - sum'| and |abs - abs'| within RTOL of the sum of |x| (a case can loosen it)


def fmt_time(t):
    if t is None:
        return "-"
    if t < 1e-6:
        return f"{t * 1e9:.0f} ns"
    if t < 1e-3:
        return f"{t * 1e6:.2f} µs" if t < 1e-5 else f"{t * 1e6:.1f} µs"
    if t < 1:
        return f"{t * 1e3:.2f} ms" if t < 1e-2 else f"{t * 1e3:.1f} ms"
    return f"{t:.2f} s"


def fmt_ratio(r):
    if r is None:
        return "-"
    return f"{r:.2f}" if r < 10 else f"{r:.1f}" if r < 100 else f"{r:.0f}"


def parse_lines(text):
    """{(case, size): {"t": seconds or None, "chk": str, "status": "ok"|"error"|"skip"}}, in order"""
    out, noise = {}, []
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) != 4:
            if line.strip():
                noise.append(line)
            continue
        case, size, t, chk = f
        if t in ("error", "skip"):
            out[(case, size)] = {"t": None, "chk": chk, "status": t}
            continue
        try:
            out[(case, size)] = {"t": float(t), "chk": chk, "status": "ok"}
        except ValueError:
            noise.append(line)
    return out, noise


def parse_chk(c):
    p = c.split(",")
    try:
        s, a = float(p[0]), float(p[1])
        rtol = float(p[2]) if len(p) > 2 else 0.0
        return s, a, rtol
    except (ValueError, IndexError):
        return None


def compare_chk(cl, cp):
    """"ok", "DIFF" or "n/a" """
    if cl == "-" or cp == "-":
        return "n/a"
    a, b = parse_chk(cl), parse_chk(cp)
    if a is None or b is None:
        return "n/a"
    rtol = max(RTOL, a[2], b[2])
    scale = max(abs(a[1]), abs(b[1]), abs(a[0]), abs(b[0]))
    for x, y in ((a[0], b[0]), (a[1], b[1])):
        if math.isnan(x) and math.isnan(y):
            continue
        if math.isinf(x) or math.isinf(y):
            if x != y:
                return "DIFF"
            continue
        if not abs(x - y) <= rtol * scale + 1e-9:
            return "DIFF"
    return "ok"


def run_script(cmd, env, label):
    """Runs one side of an area, echoing each case as it comes; returns (stdout, stderr, exit code)."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env, cwd=HERE)
    lines = []
    for line in p.stdout:
        lines.append(line)
        f = line.rstrip("\n").split("\t")
        if len(f) == 4:
            shown = fmt_time(float(f[2])) if f[2] not in ("error", "skip") else f[2]
            print(f"  {label:6} {f[0]:32} {f[1]:>18} {shown:>10}", file=sys.stderr, flush=True)
    err = p.stderr.read()
    code = p.wait()
    return "".join(lines), err, code


def find_models(args):
    models = list(args.model or [])
    if not models and os.environ.get("BENCH_MODELS"):
        models = [m for m in os.environ["BENCH_MODELS"].split(":") if m]
    if not models:
        tiny = os.path.join(ROOT, "tests", "models", "llama-mixed.gguf")
        if os.path.exists(tiny):
            models = [tiny]
    return [os.path.abspath(m) for m in models if os.path.exists(m)]


def run_area(area, args, tmp, filt, env):
    extra = find_models(args) if area == "llm" else []
    if area == "llm" and not extra:
        print("llm: no GGUF model found: skipped", file=sys.stderr)
        return []
    common = [tmp, str(args.min), str(args.repeats), filt] + extra
    print(f"{area}: Lube", file=sys.stderr, flush=True)
    lout, lerr, lcode = run_script([args.lube, os.path.join(HERE, area + ".lube")] + common, env, "lube")
    print(f"{area}: Python", file=sys.stderr, flush=True)
    pyfile = os.path.join(HERE, area + ".py")
    pout, perr, pcode = run_script([args.python, pyfile] + common, env, "python")
    lres, lnoise = parse_lines(lout)
    pres, pnoise = parse_lines(pout)
    for side, code, err, noise in (("lube", lcode, lerr, lnoise), ("python", pcode, perr, pnoise)):
        if code:
            print(f"{area}: {side} exited with {code}", file=sys.stderr)
        tail = "\n".join((noise + err.strip().splitlines())[-8:])
        if code and tail:
            print("    " + tail.replace("\n", "\n    "), file=sys.stderr)
    rows = []
    for key in list(lres) + [k for k in pres if k not in lres]:
        l, p = lres.get(key), pres.get(key)
        row = {"area": area, "case": key[0], "size": key[1],
               "lube": l["t"] if l else None, "python": p["t"] if p else None,
               "lube_chk": l["chk"] if l else "", "python_chk": p["chk"] if p else "",
               "lube_status": l["status"] if l else "missing", "python_status": p["status"] if p else "missing"}
        finish_row(row)
        rows.append(row)
    return rows


def finish_row(row):
    l, p = row["lube"], row["python"]
    row["ratio"] = p / l if l and p else None
    if row["lube_status"] == "ok" and row["python_status"] == "ok":
        row["check"] = compare_chk(row["lube_chk"], row["python_chk"])
    else:
        row["check"] = "/".join(s for s in (row["lube_status"], row["python_status"]) if s != "ok") or "n/a"


def note(row):
    """What the table shows in place of a time that isn't there."""
    out = []
    for side in ("lube", "python"):
        st = row[side + "_status"]
        if st != "ok":
            out.append(f"{side} {st}: {row[side + '_chk']}"[:110])
    return "; ".join(out)


def table(rows, md=False):
    head = ["area", "case", "size", "Lube", "Python", "Python/Lube", "checksum"]
    body = [[r["area"], r["case"], r["size"], fmt_time(r["lube"]), fmt_time(r["python"]), fmt_ratio(r["ratio"]),
             r["check"]] for r in rows]
    if md:
        lines = ["| " + " | ".join(head) + " |", "|" + "|".join(["---"] * 3 + ["---:"] * 3 + ["---"]) + "|"]
        for r, b in zip(rows, body):
            if r["ratio"] is not None and r["ratio"] < 1:
                b[5] = f"**{b[5]}**"
            extra = note(r)
            if extra:
                b[6] += f" ({extra})"
            lines.append("| " + " | ".join(str(x).replace("|", "\\|") for x in b) + " |")
        return "\n".join(lines)
    widths = [max(len(str(x)) for x in col) for col in zip(head, *body)] if body else [len(h) for h in head]
    widths[6] = 8
    lines = []
    for i, b in enumerate([head] + body):
        cells = [str(x).ljust(w) if j < 3 or j == 6 else str(x).rjust(w) for j, (x, w) in enumerate(zip(b, widths))]
        line = "  ".join(cells)
        if i > 0:
            r = rows[i - 1]
            if r["ratio"] is not None and r["ratio"] < 1:
                line += "  <- Python faster"
            extra = note(r)
            if extra:
                line += "  " + extra
        lines.append(line.rstrip())
    return "\n".join(lines)


def geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None


def summary(rows, md=False):
    lines = []
    areas = [a for a in AREAS if any(r["area"] == a for r in rows)]
    if md:
        lines += ["| area | cases | Python faster | geo. mean Python/Lube | checksums differ |", "|---|---:|---:|---:|---:|"]
    else:
        lines.append(f"{'area':10} {'cases':>6} {'Py faster':>10} {'geo. mean Py/Lube':>18} {'chk DIFF':>9}")
    for a in areas + ["all"]:
        rs = [r for r in rows if a == "all" or r["area"] == a]
        timed = [r for r in rs if r["ratio"] is not None]
        wins = sum(1 for r in timed if r["ratio"] < 1)
        diff = sum(1 for r in rs if r["check"] == "DIFF")
        gm = geomean([r["ratio"] for r in timed])
        if md:
            lines.append(f"| {a} | {len(timed)} | {wins} | {fmt_ratio(gm)} | {diff} |")
        else:
            lines.append(f"{a:10} {len(timed):>6} {wins:>10} {fmt_ratio(gm):>18} {diff:>9}")
    return "\n".join(lines)


def compare_table(rows, old_rows):
    old = {(r["area"], r["case"], r["size"]): r for r in old_rows}
    head = f"{'area':9} {'case':32} {'size':>18} {'old Lube':>10} {'new Lube':>10} {'old/new':>8} {'Python':>10} {'Py/Lube':>8}"
    lines = [head]
    gains = []
    for r in rows:
        o = old.get((r["area"], r["case"], r["size"]))
        ot = o["lube"] if o else None
        g = ot / r["lube"] if ot and r["lube"] else None
        if g:
            gains.append(g)
        mark = "  faster" if g and g > 1.1 else "  SLOWER" if g and g < 0.9 else ""
        lines.append(f"{r['area']:9} {r['case']:32} {r['size']:>18} {fmt_time(ot):>10} {fmt_time(r['lube']):>10} "
                     f"{fmt_ratio(g):>8} {fmt_time(r['python']):>10} {fmt_ratio(r['ratio']):>8}{mark}")
    lines.append(f"geo. mean old/new: {fmt_ratio(geomean(gains))} over {len(gains)} cases")
    return "\n".join(lines)


def versions(args, env):
    v = {"lube": args.lube, "python": args.python, "host": platform.platform(), "machine": platform.machine(),
         "cpus": os.cpu_count(), "date": datetime.datetime.now().isoformat(timespec="seconds"),
         "min_seconds": args.min, "repeats": args.repeats}
    code = ("import sys; print(sys.version.split()[0])\n"
            "for m in ('numpy', 'torch', 'sklearn', 'llama_cpp', 'safetensors'):\n"
            "    try:\n        mod = __import__(m); print(m, mod.__version__)\n"
            "    except Exception: print(m, 'missing')\n")
    try:
        out = subprocess.run([args.python, "-c", code], capture_output=True, text=True, env=env, timeout=120).stdout
        lines = out.split("\n")
        v["python_version"] = lines[0]
        v["libraries"] = dict(line.split(" ", 1) for line in lines[1:] if " " in line)
    except (OSError, subprocess.SubprocessError):
        pass
    return v


def losers_of(old_rows, areas):
    """{area: [case names]} where Python was faster, or the checksums differed"""
    out = {}
    for r in old_rows:
        if r["area"] in areas and (r["ratio"] is not None and r["ratio"] < 1 or r["check"] == "DIFF"):
            out.setdefault(r["area"], [])
            if r["case"] not in out[r["area"]]:
                out[r["area"]].append(r["case"])
    return out


def merge(old_rows, new_rows):
    """The old results with each re-run case's better time of the two runs, for each side."""
    new = {(r["area"], r["case"], r["size"]): r for r in new_rows}
    merged = []
    for o in old_rows:
        n = new.get((o["area"], o["case"], o["size"]))
        if not n:
            merged.append(o)
            continue
        m = dict(o)
        m["first_run"] = {"lube": o["lube"], "python": o["python"]}
        m["second_run"] = {"lube": n["lube"], "python": n["python"]}
        for side in ("lube", "python"):
            ts = [t for t in (o[side], n[side]) if t is not None]
            m[side] = min(ts) if ts else None
            if n[side + "_status"] == "ok":  # (the checksum as the second run computed it)
                m[side + "_status"] = "ok"
                m[side + "_chk"] = n[side + "_chk"]
        finish_row(m)
        merged.append(m)
    return merged


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("areas", nargs="*", help="areas to run (default: all)")
    ap.add_argument("--lube", default=os.environ.get("LUBE", os.path.join(ROOT, "lube")), help="the lube to test ($LUBE)")
    ap.add_argument("--python", default=os.environ.get("PYTHON", "python3"), help="the Python to compare with ($PYTHON)")
    ap.add_argument("--json", help="write the results here")
    ap.add_argument("--md", help="write a Markdown table here")
    ap.add_argument("--compare", metavar="OLD.json", help="show each case's Lube time against OLD.json's")
    ap.add_argument("--losers", metavar="OLD.json", help="run again only the cases Python won (or whose checksums differed) in OLD.json")
    ap.add_argument("--show", metavar="FILE.json", help="print a saved run's table (runs nothing)")
    ap.add_argument("--filter", default="", help="only the cases whose name has this in it (or =name,name)")
    ap.add_argument("--model", action="append", help="a GGUF file for the llm area (repeatable)")
    ap.add_argument("--min", type=float, default=0.05, help="seconds one measurement takes at least (0.05)")
    ap.add_argument("--repeats", type=int, default=5, help="measurements of each case; the best counts (5)")
    ap.add_argument("--tmp", help="a folder for the files the cases write (default: a new temporary one)")
    args = ap.parse_args()

    if args.show:
        with open(args.show) as f:
            data = json.load(f)
        print(table(data["results"]))
        print()
        print(summary(data["results"]))
        return

    areas = args.areas or AREAS
    for a in areas:
        if a not in AREAS:
            ap.error(f"no area {a!r} (there are {', '.join(AREAS)})")
    args.lube = os.path.abspath(args.lube) if os.path.sep in args.lube else args.lube
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    tmp = args.tmp or tempfile.mkdtemp(prefix="vs_python_")
    os.makedirs(tmp, exist_ok=True)

    old_rows = None
    if args.losers:
        with open(args.losers) as f:
            old_rows = json.load(f)["results"]
        todo = losers_of(old_rows, areas)
        areas = [a for a in areas if a in todo]

    rows = []
    for area in areas:
        filt = args.filter
        if args.losers:
            filt = "=" + ",".join(todo[area])
        rows += run_area(area, args, tmp, filt, env)

    meta = versions(args, env)
    if args.losers:
        print(f"\nRun again: the cases Python won in {args.losers}")
        old = {(r["area"], r["case"], r["size"]): r for r in old_rows}
        print(f"{'area':9} {'case':32} {'size':>18} {'1st Py/Lube':>12} {'2nd Py/Lube':>12}  2nd: Lube, Python")
        for r in rows:
            o = old.get((r["area"], r["case"], r["size"]))
            print(f"{r['area']:9} {r['case']:32} {r['size']:>18} {fmt_ratio(o['ratio'] if o else None):>12} "
                  f"{fmt_ratio(r['ratio']):>12}  {fmt_time(r['lube'])}, {fmt_time(r['python'])}")
        rows = merge(old_rows, rows)
        meta["losers_rerun_of"] = args.losers
        print()

    print(table(rows))
    print()
    print(summary(rows))
    wins = sorted((r for r in rows if r["ratio"] is not None and r["ratio"] < 1), key=lambda r: r["ratio"])
    if wins:
        print(f"\nPython faster in {len(wins)} cases, worst first:")
        for r in wins:
            print(f"  {r['area']:9} {r['case']:32} {r['size']:>18}  {fmt_ratio(r['ratio']):>6}x  "
                  f"Lube {fmt_time(r['lube'])}, Python {fmt_time(r['python'])}")
    diffs = [r for r in rows if r["check"] == "DIFF"]
    if diffs:
        print(f"\nChecksums differ in {len(diffs)} cases:")
        for r in diffs:
            print(f"  {r['area']:9} {r['case']:32} {r['size']:>18}  lube {r['lube_chk']}  python {r['python_chk']}")

    if args.compare:
        with open(args.compare) as f:
            print()
            print(compare_table(rows, json.load(f)["results"]))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "results": rows}, f, indent=1)
    if args.md:
        with open(args.md, "w") as f:
            lib = meta.get("libraries", {})
            f.write(f"# Lube against Python\n\n{meta['date']}, {meta['host']}, {meta['cpus']} cores. "
                    f"Python {meta.get('python_version', '?')}, " + ", ".join(f"{k} {v}" for k, v in lib.items()) +
                    f". Lube: `{meta['lube']}`.\n\nPython/Lube above 1: Lube is faster (bold: Python is).\n\n")
            f.write(summary(rows, md=True) + "\n\n" + table(rows, md=True) + "\n")


if __name__ == "__main__":
    main()
