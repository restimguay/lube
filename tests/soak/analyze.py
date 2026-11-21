# analyze.py DIR (build/soak): what tests/soak/run.sh's files say: rounds, checksums, each workload's speed early and late, memory's trend
import sys, csv, statistics as st, re

d = sys.argv[1]
lines = open(f"{d}/rounds.txt").read().split("\n")
names = lines[0].split()[3:]
rounds = []
for l in lines[1:]:
    if not l.startswith("R "):
        continue
    head, chk = l.split(" chk=")
    p = head.split()
    rounds.append((int(p[1]), float(p[2]), [float(x) for x in p[3:]], chk))
print(f"rounds: {len(rounds)} in {rounds[-1][1]:.0f} s ({rounds[-1][1] / len(rounds):.3f} s each)")
chks = {r[3] for r in rounds}
print(f"checksums: {'all identical' if len(chks) == 1 else f'{len(chks)} different!'}")
if len(chks) > 1:
    first = rounds[0][3]
    for r in rounds:
        if r[3] != first:
            print("  first different: round", r[0], r[3][:200])
            break


def window(lo, hi):
    return [r for r in rounds if lo <= r[1] < hi]


end = rounds[-1][1]
if end >= 720:  # minutes 1-6 against the last 5
    early, late, what = window(60, 360), window(end - 300, end + 1), "minutes 1-6 vs the last 5"
else:  # (a short run: its first half against its second)
    early, late, what = rounds[: (len(rounds) + 1) // 2], rounds[len(rounds) // 2:], "first half vs second half"
print(f"\nmedian ms per workload: {what} ({len(early)} and {len(late)} rounds)")
for i, n in enumerate(names):
    e = st.median(r[2][i] for r in early)
    l = st.median(r[2][i] for r in late)
    print(f"  {n:9s} {e:8.1f} {l:8.1f}  {(l / e - 1) * 100:+6.1f}%")
e = st.median(sum(r[2]) for r in early)
l = st.median(sum(r[2]) for r in late)
print(f"  {'round':9s} {e:8.1f} {l:8.1f}  {(l / e - 1) * 100:+6.1f}%")

# per 10 minutes: rounds done, median round time, load
mem = list(csv.DictReader(open(f"{d}/mem.csv")))
print("\nper 10 minutes: rounds, median round ms, rss MB (min/median/max), load average")
for k in range(0, int(end) + 1, 600):
    rs = window(k, k + 600)
    ms = [m for m in mem if k <= int(m["t"]) < k + 600]
    if not rs or not ms:
        continue
    rss = [int(m["rss_kb"]) / 1024 for m in ms]
    load = [float(m["load1"]) for m in ms]
    print(f"  {k // 60:3d}-{k // 60 + 10:<3d} min  {len(rs):5d}  {st.median(sum(r[2]) for r in rs):7.1f}  "
          f"{min(rss):6.0f} {st.median(rss):6.0f} {max(rss):6.0f}  {st.mean(load):5.2f}")


def slope(xs, ys):  # least squares: units of y per unit of x
    mx, my = st.mean(xs), st.mean(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


after = [m for m in mem if int(m["t"]) >= 300]
if len(after) > 2:
    s = slope([int(m["t"]) for m in after], [int(m["rss_kb"]) / 1024 for m in after])
    print(f"\nrss trend after minute 5: {s * 3600:+.1f} MB per hour")
foot = list(csv.DictReader(open(f"{d}/foot.csv")))


def mb(t):
    m = re.match(r"([\d.]+)([KMG]?B)", t)
    return float(m.group(1)) * {"B": 1 / 2**20, "KB": 1 / 1024, "MB": 1, "GB": 1024}[m.group(2)]


if foot:
    fs = [(int(f["t"]), mb(f["phys_footprint"])) for f in foot if f["phys_footprint"]]
    print(f"footprint: first {fs[0][1]:.0f} MB, min {min(f for _, f in fs):.0f}, median {st.median(f for _, f in fs):.0f}, "
          f"max {max(f for _, f in fs):.0f}, last {fs[-1][1]:.0f}; peak {foot[-1]['phys_footprint_peak']}")
    fa = [(t, f) for t, f in fs if t >= 300]
    if len(fa) > 2:
        print(f"footprint trend after minute 5: {slope([t for t, _ in fa], [f for _, f in fa]) * 3600:+.1f} MB per hour")
try:
    print("\nleaks (live):\n" + open(f"{d}/leaks_live.txt").read())
except FileNotFoundError:
    pass
