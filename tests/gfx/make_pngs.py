#!/usr/bin/env python3
"""Writes the PNG files tests/gfx.lube reads, with the pixels each should give.

Every kind of PNG: grayscale, RGB, palette, gray and alpha, RGBA; 1 to 16
bits; transparent colors (tRNS); interlaced (Adam7) or not; each of the five
row filters; stored, fixed and dynamic deflate blocks. For each NAME.png,
NAME.txt has the pixels expected (0xAARRGGBB, rows top first).

In bad/, files that are damaged in every way the reader checks for: each
should be refused, with the reason.

    python3 tests/gfx/make_pngs.py      (from the repository's folder)
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 9, 7  # (odd sizes: every Adam7 pass, and rows that don't fill a byte)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pack(samples, depth):
    """A row's samples at depth bits each, as bytes."""
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filtered(rows, bpp):
    """Each row filtered, cycling through the five filters."""
    out, prior = bytearray(), bytes(len(rows[0])) if rows else b""
    for y, row in enumerate(rows):
        f = y % 5
        out.append(f)
        for i, x in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prior[i]
            c = prior[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            out.append((x - pred) & 255)
        prior = row
    return bytes(out)


PASSES = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def write(name, ctype, depth, samples_at, expect_at, plte=None, trns=None, interlace=0, compress="dynamic"):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, depth * channels // 8)
    if interlace:
        raw = b""
        for (px, py, dx, dy) in PASSES:
            xs, ys = list(range(px, W, dx)), list(range(py, H, dy))
            if xs and ys:
                raw += filtered([pack(sum((samples_at(x, y) for x in xs), []), depth) for y in ys], bpp)
    else:
        raw = filtered([pack(sum((samples_at(x, y) for x in range(W)), []), depth) for y in range(H)], bpp)
    if compress == "stored":
        data = zlib.compress(raw, 0)
    elif compress == "fixed":
        c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_FIXED)
        data = c.compress(raw) + c.flush()
    else:
        data = zlib.compress(raw, 9)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, depth, ctype, 0, 0, interlace))
    out += chunk(b"tEXt", b"Comment\x00made by make_pngs.py")  # (a chunk to skip)
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    half = len(data) // 2  # (the pixels in two IDAT chunks)
    out += chunk(b"IDAT", data[:half]) + chunk(b"IDAT", data[half:]) + chunk(b"IEND", b"")
    with open(os.path.join(HERE, name + ".png"), "wb") as f:
        f.write(out)
    with open(os.path.join(HERE, name + ".txt"), "w") as f:
        for y in range(H):
            f.write(" ".join("%08x" % expect_at(x, y) for x in range(W)) + "\n")


def argb(a, r, g, b):
    return a << 24 | r << 16 | g << 8 | b


def main():
    for depth in (1, 2, 4, 8, 16):  # grayscale
        top = (1 << depth) - 1
        val = lambda x, y, top=top: (x * 3 + y * 5) % (top + 1)
        scale = lambda v, depth=depth: v >> 8 if depth == 16 else v if depth == 8 else v * (255 // ((1 << depth) - 1))
        write("gray%d" % depth, 0, depth, lambda x, y: [val(x, y)],
              lambda x, y: argb(255, scale(val(x, y)), scale(val(x, y)), scale(val(x, y))))
    key = 2  # gray with a transparent value
    val = lambda x, y: (x + y) % 4
    write("gray2_trns", 0, 2, lambda x, y: [val(x, y)],
          lambda x, y: argb(0 if val(x, y) == key else 255, val(x, y) * 85, val(x, y) * 85, val(x, y) * 85),
          trns=struct.pack(">H", key))
    for depth in (8, 16):  # RGB, and RGB with a transparent color
        m = 257 if depth == 16 else 1
        rgb = lambda x, y: [x * 28, y * 36, (x * y * 7) % 256]
        write("rgb%d" % depth, 2, depth, lambda x, y, m=m: [v * m for v in rgb(x, y)],
              lambda x, y: argb(255, *rgb(x, y)))
        write("rgb%d_trns" % depth, 2, depth, lambda x, y, m=m: [v * m for v in rgb(x, y)],
              lambda x, y: argb(0 if rgb(x, y) == [56, 36, 14] else 255, *rgb(x, y)),
              trns=struct.pack(">HHH", 56 * m, 36 * m, 14 * m))
    for depth in (1, 2, 4, 8):  # palette, some entries partly transparent
        n = 1 << depth
        colors = [(i * 37 % 256, i * 91 % 256, i * 53 % 256) for i in range(n)]
        alphas = [255 - i * 60 for i in range(min(n, 4))]
        idx = lambda x, y, n=n: (x * 2 + y) % n
        al = lambda i, alphas=alphas: alphas[i] if i < len(alphas) else 255
        write("palette%d" % depth, 3, depth, lambda x, y: [idx(x, y)],
              lambda x, y, colors=colors: argb(al(idx(x, y)), *colors[idx(x, y)]),
              plte=b"".join(bytes(c) for c in colors), trns=bytes(alphas))
    for depth in (8, 16):  # gray and alpha; RGBA
        m = 257 if depth == 16 else 1
        ga = lambda x, y: [x * 30, 255 - y * 40]
        write("gray_alpha%d" % depth, 4, depth, lambda x, y, m=m: [v * m for v in ga(x, y)],
              lambda x, y: argb(ga(x, y)[1], ga(x, y)[0], ga(x, y)[0], ga(x, y)[0]))
        rgba = lambda x, y: [x * 28, y * 36, 200, (x + y) * 15]
        write("rgba%d" % depth, 6, depth, lambda x, y, m=m: [v * m for v in rgba(x, y)],
              lambda x, y: argb(rgba(x, y)[3], *rgba(x, y)[:3]))
    rgba = lambda x, y: [x * 28, y * 36, 200, 255 - (x + y) * 15]
    for compress in ("stored", "fixed", "dynamic"):  # interlaced, each kind of block
        write("interlaced_" + compress, 6, 8, rgba, lambda x, y: argb(rgba(x, y)[3], *rgba(x, y)[:3]),
              interlace=1, compress=compress)
    write("interlaced_gray1", 0, 1, lambda x, y: [(x ^ y) & 1], lambda x, y: argb(255, *[255 * ((x ^ y) & 1)] * 3),
          interlace=1)


class Bits:
    """Deflate's bits: values from their lowest bit; Huffman codes from their highest."""

    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, value, count):
        for i in range(count):
            self.acc |= ((value >> i) & 1) << self.n
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def code(self, code, length):
        for i in reversed(range(length)):
            self.put((code >> i) & 1, 1)

    def align(self):
        if self.n:
            self.put(0, 8 - self.n)

    def done(self):
        self.align()
        return bytes(self.out)


def canonical(lengths):
    """A Huffman code's codes from their lengths: {symbol: (code, length)}."""
    codes, code = {}, 0
    for length in range(1, 16):
        for sym, l in enumerate(lengths):
            if l == length:
                codes[sym] = (code, length)
                code += 1
        code <<= 1
    return codes


def fixed(bits, sym):
    """A literal/length symbol in the fixed code."""
    if sym < 144:
        bits.code(0x30 + sym, 8)
    elif sym < 256:
        bits.code(0x190 + sym - 144, 9)
    elif sym < 280:
        bits.code(sym - 256, 7)
    else:
        bits.code(0xC0 + sym - 280, 8)


# a complete code for the code lengths: 8, 16, 17, 18 with 3 bits; 0 to 7 with 4
CL_LENGTHS = [4] * 8 + [3] + [0] * 7 + [3, 3, 3]
CL = canonical(CL_LENGTHS)
ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


def dynamic(nlen, ndist, steps, cl_lengths=CL_LENGTHS, after=None):
    """A last dynamic block: its header, the code lengths (steps: a symbol, or (symbol, extra bits, their
    value)), then whatever after(bits) writes."""
    bits = Bits()
    bits.put(1, 1)
    bits.put(2, 2)
    bits.put(nlen - 257, 5)
    bits.put(ndist - 1, 5)
    bits.put(19 - 4, 4)
    for sym in ORDER:
        bits.put(cl_lengths[sym], 3)
    codes = canonical(cl_lengths)
    for step in steps:
        sym, extra, value = step if isinstance(step, tuple) else (step, 0, 0)
        bits.code(*codes[sym])
        bits.put(value, extra)
    if after:
        after(bits)
    return bits.done()


def lengths_steps(lengths):
    """Code lengths (each 0 to 8) as symbols, one each."""
    return list(lengths)


def write_bad(name, data=None, ihdr=(1, 1, 8, 0, 0, 0, 0), idat=True, raw=None):
    """bad/NAME.png: a PNG with this header, and data (zlib's two bytes and a deflate stream) as its pixels."""
    if data is None:
        data = zlib.compress(raw if raw is not None else b"\x00\x80")
    out = b"\x89PNG\r\n\x1a\n"
    if ihdr:
        out += chunk(b"IHDR", struct.pack(">IIBBBBB", *ihdr))
    if idat:
        out += chunk(b"IDAT", data)
    out += chunk(b"IEND", b"")
    os.makedirs(os.path.join(HERE, "bad"), exist_ok=True)
    with open(os.path.join(HERE, "bad", name + ".png"), "wb") as f:
        f.write(out)


def bad():
    z = b"\x78\x01"  # (zlib's header: deflate, no dictionary)
    # the header
    write_bad("header_color_5", ihdr=(1, 1, 8, 5, 0, 0, 0))
    write_bad("header_gray_3_bits", ihdr=(1, 1, 3, 0, 0, 0, 0))
    write_bad("header_rgb_4_bits", ihdr=(1, 1, 4, 2, 0, 0, 0))
    write_bad("header_interlace_2", ihdr=(1, 1, 8, 0, 0, 0, 2))
    write_bad("header_width_0", ihdr=(0, 1, 8, 0, 0, 0, 0))
    write_bad("header_missing", ihdr=None)
    write_bad("size_too_big", ihdr=(20000, 20000, 8, 0, 0, 0, 0))
    write_bad("size_more_than_its_data", ihdr=(8000, 8000, 8, 0, 0, 0, 0))
    write_bad("no_pixels", idat=False)
    write_bad("filter_7", raw=b"\x07\x80")
    # zlib's own header
    write_bad("zlib_not_deflate", b"\x79\x01" + Bits().done())
    write_bad("zlib_check", b"\x78\x02")
    write_bad("zlib_dictionary", b"\x78\x20\x00\x00\x00\x00")
    write_bad("zlib_cut_short", z)
    # blocks of no kind, and stored blocks
    b = Bits()
    b.put(1, 1)
    b.put(3, 2)
    write_bad("block_type_3", z + b.done())

    def stored(length, nlength, body):
        b = Bits()
        b.put(1, 1)
        b.put(0, 2)
        return z + b.done() + struct.pack("<HH", length, nlength) + body

    write_bad("stored_cut_short", stored(2, 0xFFFD, b"")[:5])
    write_bad("stored_length_check", stored(2, 0, b"\x00\x80"))
    write_bad("stored_past_the_end", stored(10, 0xFFF5, b"\x00\x80"))
    write_bad("stored_too_much", stored(3, 0xFFFC, b"\x00\x80\x80"))
    write_bad("stored_too_little", stored(1, 0xFFFE, b"\x00"))
    # fixed blocks

    def fixed_block(*syms_and_bits):
        b = Bits()
        b.put(1, 1)
        b.put(1, 2)
        for item in syms_and_bits:
            if isinstance(item, tuple):
                b.code(*item)
            else:
                fixed(b, item)
        return z + b.done()

    write_bad("fixed_length_286", fixed_block(0, 286))
    write_bad("fixed_before_the_start", fixed_block(257, (0, 5)))
    write_bad("fixed_distance_30", fixed_block(0, 257, (30, 5), (0, 10)))
    # dynamic blocks
    write_bad("dynamic_hlit_287", z + dynamic(287, 1, []))
    write_bad("dynamic_hdist_31", z + dynamic(257, 31, []))
    write_bad("dynamic_code_lengths_too_many", z + dynamic(257, 1, [], cl_lengths=[1, 1, 1] + [0] * 16))
    write_bad("dynamic_code_lengths_too_few", z + dynamic(257, 1, [], cl_lengths=[1] + [0] * 18))
    write_bad("dynamic_repeat_first", z + dynamic(257, 1, [(16, 2, 0)]))
    write_bad("dynamic_repeat_past", z + dynamic(257, 1, [(18, 7, 127), (18, 7, 127)]))
    write_bad("dynamic_no_end", z + dynamic(257, 1, [8] * 256 + [0, 1]))
    write_bad("dynamic_lengths_too_many", z + dynamic(257, 1, [1] * 257 + [1]))
    write_bad("dynamic_lengths_too_few", z + dynamic(257, 1, [2] + [0] * 255 + [2, 1]))
    write_bad("dynamic_distances_too_many", z + dynamic(257, 3, [1] + [0] * 255 + [1, 1, 1, 1]))
    write_bad("dynamic_distances_too_few", z + dynamic(257, 2, [1] + [0] * 255 + [1, 2, 2]))
    lit = canonical([2] + [0] * 255 + [2, 1])  # (0: 2 bits; the end: 2; length 3: 1. One distance, 0: 1 bit)
    write_bad("dynamic_unused_distance", z + dynamic(258, 1, [2] + [0] * 255 + [2, 1, 1],
                                                     after=lambda b: (b.code(*lit[0]), b.code(*lit[257]), b.code(1, 1))))
    write_bad("dynamic_cut_short", (z + dynamic(258, 1, [2] + [0] * 255 + [2, 1, 1]))[:-1])


main()
bad()
