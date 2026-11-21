"""money.py: Python's decimal, rounding half up to the cent (money.lube does the same with Lube's Money): making
amounts, adding, multiplying and rounding, formatting, splitting and allocating, converting. The size is the
number of amounts. Each operation that Lube rounds is quantized here, as a program keeping cents must."""
from harness import bench, bench_chk  # (first: see harness.py)
import random
from decimal import ROUND_HALF_UP, Decimal


CENT = Decimal("0.01")
ONE = Decimal(1)
RATE_TAX = Decimal("0.0725")
RATE_JPY = Decimal("151.234")
RATE_EUR = Decimal("0.9234")
RATE_INTEREST = Decimal("1.01")
SWAP = str.maketrans(",.", ".,")


def money(x):
    """m(x): a Number as the decimal it is written as, rounded half up to the cent"""
    return Decimal(repr(x) if isinstance(x, float) else str(x)).quantize(CENT, ROUND_HALF_UP)


def cents(d):
    return int(d * 100)


def split(amount, n):
    units = int(amount / CENT)
    base, extra = divmod(units, n)
    return [Decimal(base + (1 if i < extra else 0)) * CENT for i in range(n)]


def allocate(amount, ratios):
    units = int(amount / CENT)
    total = sum(ratios)
    raw = [units * r / total for r in ratios]
    shares = [int(x) for x in raw]
    left = units - sum(shares)
    for i in sorted(range(len(ratios)), key=lambda i: raw[i] - shares[i], reverse=True)[:left]:
        shares[i] += 1
    return [Decimal(s) * CENT for s in shares]


def weighted(xs):
    s = 0
    for i, x in enumerate(xs):
        s += (i + 1) * x
    return s


def main():
    for n in [1000, 100000]:
        random.seed(5)
        cs = [random.randint(1, 10000000) for i in range(n)]
        nums = [c / 100 for c in cs]
        texts = [f"{c // 100}.{c % 100:02d}" for c in cs]
        ms = [money(x) for x in nums]

        bench("from_number", n, lambda: [money(x) for x in nums])
        bench("from_text", n, lambda: [Decimal(t) for t in texts])

        def add_loop():
            t = Decimal(0)
            for x in ms:
                t += x
            return t
        bench("add_loop", n, add_loop)
        bench("sum_builtin", n, lambda: sum(ms, Decimal(0)))
        bench("add_number", n, lambda: [x + 1 for x in ms])
        bench("multiply_round", n, lambda: [(x * RATE_TAX).quantize(CENT, ROUND_HALF_UP) for x in ms])
        bench("multiply_int", n, lambda: [x * 3 for x in ms])
        bench("divide_round", n, lambda: [(x / 3).quantize(CENT, ROUND_HALF_UP) for x in ms])

        def tax_total():
            t = Decimal(0)
            for x in ms:
                t += (x * RATE_TAX).quantize(CENT, ROUND_HALF_UP)
            return t
        bench("tax_total", n, tax_total)

        def compound_interest():
            t = Decimal(0)
            for x in ms:
                b = x
                for k in range(12):
                    b = (b * RATE_INTEREST).quantize(CENT, ROUND_HALF_UP)
                t += b
            return t
        bench("compound_interest", n, compound_interest)
        bench("compare_max", n, lambda: max(ms))
        bench_chk("sort", n, lambda: sorted(ms), lambda r: weighted([cents(x) for x in r]))
        bench("round_to_units", n, lambda: [x.quantize(ONE, ROUND_HALF_UP) for x in ms])
        bench("format_default", n, lambda: [f"${x:,.2f}" for x in ms])
        bench("format_custom", n, lambda: ["$" + f"{x:,.2f}".translate(SWAP) for x in ms])
        bench_chk("split_7", n, lambda: [split(x, 7) for x in ms],
                  lambda r: [weighted([cents(y) for y in s]) for s in r])
        bench_chk("allocate_5", n, lambda: [allocate(x, [1, 2, 3, 5, 8]) for x in ms],
                  lambda r: [weighted([cents(y) for y in s]) for s in r])
        bench_chk("convert_to_jpy", n, lambda: [(x * RATE_JPY).quantize(ONE, ROUND_HALF_UP) for x in ms],
                  lambda r: [int(y) for y in r])
        bench("convert_to_eur", n, lambda: [(x * RATE_EUR).quantize(CENT, ROUND_HALF_UP) for x in ms])
        bench("units", n, lambda: sum(cents(x) for x in ms))


if __name__ == "__main__":
    main()
