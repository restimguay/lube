/*
 * png.c (part of gfx.c) - reading and writing PNG images, with the
 * compression they use (zlib's deflate) written here too.
 *
 * Reading: every kind of PNG: grayscale, RGB, palette, with alpha or a
 * transparent color, 1 to 16 bits, interlaced or not. Writing: 8-bit RGBA,
 * each row filtered the way that looks smallest, compressed with LZ77 and
 * the fixed Huffman codes.
 */

/* ---------- checksums ---------- */

static uint32_t crc_table[256];

static uint32_t crc32_of(const unsigned char *p, size_t n, uint32_t crc) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32_of(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

/* ---------- inflate ---------- */

typedef struct {
    const unsigned char *in;
    size_t inlen, pos;
    uint32_t bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t outlen, outcap;   /* (the size a PNG says it is: no more) */
    int bad;
} Inflate;

typedef struct {
    short count[16];         /* codes of each length */
    short symbol[320];       /* the symbols, by code */
} Huffman;

static int need_bits(Inflate *s, int n) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < n) {
        if (s->pos >= s->inlen) {
            s->bad = 1;
            return 0;
        }
        v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> n;
    s->bitcnt -= n;
    return (int)(v & ((1u << n) - 1));
}

static int decode_symbol(Inflate *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= need_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->bad = 1;
    return -1;
}

/* A Huffman code from its codes' lengths: 0 if it is complete or (allowed for a single distance) incomplete. */
static int build_huffman(Huffman *h, const short *length, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0) return -1;  /* (more codes than there is room for) */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
    return left;
}

static void put_byte(Inflate *s, unsigned char b) {
    if (s->outlen >= s->outcap) {
        s->bad = 1;
        return;
    }
    s->out[s->outlen++] = b;
}

static const short LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                             769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void inflate_codes(Inflate *s, const Huffman *lencode, const Huffman *distcode) {
    for (;;) {
        int sym = decode_symbol(s, lencode);
        if (s->bad || sym == 256) return;
        if (sym < 256) {
            put_byte(s, (unsigned char)sym);
            continue;
        }
        sym -= 257;
        if (sym >= 29) {
            s->bad = 1;
            return;
        }
        int len = LEN_BASE[sym] + need_bits(s, LEN_EXTRA[sym]);
        int d = decode_symbol(s, distcode);
        if (s->bad || d < 0 || d >= 30) {
            s->bad = 1;
            return;
        }
        size_t dist = DIST_BASE[d] + (size_t)need_bits(s, DIST_EXTRA[d]);
        if (dist > s->outlen) {
            s->bad = 1;
            return;
        }
        while (len-- > 0 && !s->bad) put_byte(s, s->out[s->outlen - dist]);
    }
}

static void inflate_block_stored(Inflate *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->inlen) {
        s->bad = 1;
        return;
    }
    unsigned len = s->in[s->pos] | (unsigned)s->in[s->pos + 1] << 8;
    unsigned nlen = s->in[s->pos + 2] | (unsigned)s->in[s->pos + 3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xFFFF) || s->pos + len > s->inlen) {
        s->bad = 1;
        return;
    }
    while (len-- && !s->bad) put_byte(s, s->in[s->pos++]);
}

static void inflate_block_fixed(Inflate *s) {
    static Huffman lencode, distcode;
    static int built;
    if (!built) {  /* (the same tables for every image: made once; harmless if two threads both make them) */
        short lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        build_huffman(&lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        build_huffman(&distcode, lengths, 30);
        built = 1;
    }
    inflate_codes(s, &lencode, &distcode);
}

static void inflate_block_dynamic(Inflate *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huffman lencode, distcode;
    int nlen = need_bits(s, 5) + 257, ndist = need_bits(s, 5) + 1, ncode = need_bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30) {
        s->bad = 1;
        return;
    }
    int i = 0;
    for (; i < ncode; i++) lengths[order[i]] = (short)need_bits(s, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (build_huffman(&lencode, lengths, 19) != 0) {
        s->bad = 1;
        return;
    }
    for (i = 0; i < nlen + ndist && !s->bad;) {
        int sym = decode_symbol(s, &lencode);
        if (sym < 16) {
            lengths[i++] = (short)sym;
            continue;
        }
        short len = 0;
        int repeat;
        if (sym == 16) {
            if (i == 0) {
                s->bad = 1;
                return;
            }
            len = lengths[i - 1];
            repeat = 3 + need_bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + need_bits(s, 3);
        } else {
            repeat = 11 + need_bits(s, 7);
        }
        if (i + repeat > nlen + ndist) {
            s->bad = 1;
            return;
        }
        while (repeat--) lengths[i++] = len;
    }
    if (s->bad || lengths[256] == 0) {
        s->bad = 1;
        return;
    }
    int err = build_huffman(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        s->bad = 1;
        return;
    }
    err = build_huffman(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        s->bad = 1;
        return;
    }
    inflate_codes(s, &lencode, &distcode);
}

/* A zlib stream's bytes, into out (outcap of them, exactly). 0 if it is not one. */
static int zlib_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap) {
    if (inlen < 2 || (in[0] & 15) != 8 || (in[0] << 8 | in[1]) % 31 != 0 || (in[1] & 32)) return 0;
    Inflate s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = inlen;
    s.pos = 2;
    s.out = out;
    s.outcap = outcap;
    int last;
    do {
        last = need_bits(&s, 1);
        int type = need_bits(&s, 2);
        if (type == 0) inflate_block_stored(&s);
        else if (type == 1) inflate_block_fixed(&s);
        else if (type == 2) inflate_block_dynamic(&s);
        else s.bad = 1;
    } while (!last && !s.bad);
    return !s.bad && s.outlen == outcap;
}

/* ---------- deflate (LZ77, fixed Huffman codes) ---------- */

typedef struct {
    unsigned char *data;
    size_t len, cap;
    uint32_t bits;
    int nbits;
} Out;

static void out_byte(Out *o, unsigned char b) {
    if (o->len == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 4096;
        unsigned char *d = (unsigned char *)realloc(o->data, o->cap);
        if (!d) abort();  /* LCOV_EXCL_LINE: out of memory */
        o->data = d;
    }
    o->data[o->len++] = b;
}

static void out_bits(Out *o, uint32_t v, int n) {  /* (least significant first, as deflate wants) */
    o->bits |= v << o->nbits;
    o->nbits += n;
    while (o->nbits >= 8) {
        out_byte(o, (unsigned char)o->bits);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

static void out_code(Out *o, uint32_t code, int n) {  /* a Huffman code: most significant first */
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    out_bits(o, r, n);
}

static void out_literal(Out *o, int sym) {
    if (sym < 144) out_code(o, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256) out_code(o, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280) out_code(o, (uint32_t)(sym - 256), 7);
    else out_code(o, 0xC0 + (uint32_t)(sym - 280), 8);
}

static void out_match(Out *o, int len, int dist) {
    int i = 28;
    while (LEN_BASE[i] > len) i--;
    out_literal(o, 257 + i);
    out_bits(o, (uint32_t)(len - LEN_BASE[i]), LEN_EXTRA[i]);
    int d = 29;
    while (DIST_BASE[d] > dist) d--;
    out_code(o, (uint32_t)d, 5);
    out_bits(o, (uint32_t)(dist - DIST_BASE[d]), DIST_EXTRA[d]);
}

#define HASH_BITS 15
#define WINDOW 32768

/* A zlib stream of the bytes. */
static void zlib_deflate(const unsigned char *in, size_t n, Out *o) {
    out_byte(o, 0x78);
    out_byte(o, 0x01);
    out_bits(o, 1, 1);  /* one block, the last */
    out_bits(o, 1, 2);  /* fixed codes */
    int *head = (int *)malloc(sizeof(int) << HASH_BITS), *prev = (int *)malloc(sizeof(int) * WINDOW);
    if (!head || !prev) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int i = 0; i < 1 << HASH_BITS; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)in[i] << 10 ^ (uint32_t)in[i + 1] << 5 ^ in[i + 2]) & ((1u << HASH_BITS) - 1);
            int cand = head[h], chain = 32;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (cand >= 0 && i - (size_t)cand <= WINDOW && chain--) {
                size_t len = 0;
                while (len < maxlen && in[(size_t)cand + len] == in[i + len]) len++;
                if ((int)len > best) {
                    best = (int)len;
                    best_dist = (int)(i - (size_t)cand);
                    if (len == maxlen) break;
                }
                cand = prev[cand % WINDOW];
            }
            prev[i % WINDOW] = head[h];
            head[h] = (int)i;
        }
        if (best >= 3) {
            out_match(o, best, best_dist);
            for (size_t k = 1; k < (size_t)best; k++) {  /* (the bytes it covered go in the hash too) */
                size_t j = i + k;
                if (j + 3 <= n) {
                    uint32_t h = ((uint32_t)in[j] << 10 ^ (uint32_t)in[j + 1] << 5 ^ in[j + 2]) & ((1u << HASH_BITS) - 1);
                    prev[j % WINDOW] = head[h];
                    head[h] = (int)j;
                }
            }
            i += (size_t)best;
        } else {
            out_literal(o, in[i++]);
        }
    }
    out_literal(o, 256);
    if (o->nbits) out_bits(o, 0, 8 - o->nbits);
    uint32_t a = adler32_of(in, n);
    for (int k = 3; k >= 0; k--) out_byte(o, (unsigned char)(a >> (8 * k)));
    free(head);
    free(prev);
}

/* ---------- PNG ---------- */

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undoes the filters of h rows of `stride` bytes (each after its filter byte), bpp bytes a pixel. 0 if bad. */
static int unfilter(unsigned char *raw, int h, size_t stride, int bpp) {
    unsigned char *prior = NULL;
    for (int y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (stride + 1), *cur = row + 1;
        int type = row[0];
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? cur[i - (size_t)bpp] : 0, b = prior ? prior[i] : 0,
                c = prior && i >= (size_t)bpp ? prior[i - (size_t)bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: cur[i] = (unsigned char)(cur[i] + a); break;
            case 2: cur[i] = (unsigned char)(cur[i] + b); break;
            case 3: cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (unsigned char)(cur[i] + paeth(a, b, c)); break;
            default: return 0;
            }
        }
        prior = cur;
    }
    return 1;
}

typedef struct {
    int depth, color, channels;
    uint32_t palette[256];
    int npalette;
    int key_r, key_g, key_b;  /* a transparent color (tRNS of gray or RGB), or -1 */
} PngInfo;

/* Sample i of a row: depth bits each (16-bit: its high byte). */
static int sample(const unsigned char *row, size_t i, int depth) {
    if (depth == 8) return row[i];
    if (depth == 16) return row[2 * i];
    size_t bit = i * (size_t)depth;
    return (row[bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1 << depth) - 1);
}

static int sample16(const unsigned char *row, size_t i) { return row[2 * i] << 8 | row[2 * i + 1]; }

/* Pixels of one unfiltered row (w of them) into out, every `step`-th from out[0]. */
static void png_row(const PngInfo *pi, const unsigned char *row, int w, uint32_t *out, int step) {
    int scale = pi->depth >= 8 ? 1 : 255 / ((1 << pi->depth) - 1);
    for (int x = 0; x < w; x++) {
        uint32_t r, g, b, a = 255;
        size_t i = (size_t)x * (size_t)pi->channels;
        if (pi->color == 3) {  /* palette */
            int v = sample(row, i, pi->depth);
            out[(size_t)x * (size_t)step] = v < pi->npalette ? pi->palette[v] : 0xFF000000u;
            continue;
        }
        r = (uint32_t)sample(row, i, pi->depth);
        if (pi->color == 0 || pi->color == 4) {  /* gray, perhaps with alpha */
            int key = pi->depth == 16 ? sample16(row, i) : (int)r;
            r = g = b = r * (uint32_t)scale;
            if (pi->color == 4) a = (uint32_t)sample(row, i + 1, pi->depth);
            else if (key == pi->key_r) a = 0;
        } else {  /* RGB, perhaps with alpha */
            g = (uint32_t)sample(row, i + 1, pi->depth);
            b = (uint32_t)sample(row, i + 2, pi->depth);
            if (pi->color == 6) a = (uint32_t)sample(row, i + 3, pi->depth);
            else if ((pi->depth == 16 ? sample16(row, i) : (int)r) == pi->key_r &&
                     (pi->depth == 16 ? sample16(row, i + 1) : (int)g) == pi->key_g &&
                     (pi->depth == 16 ? sample16(row, i + 2) : (int)b) == pi->key_b)
                a = 0;
        }
        out[(size_t)x * (size_t)step] = a << 24 | r << 16 | g << 8 | b;
    }
}

#define MAX_PIXELS (64u * 1024u * 1024u)

/* A PNG's pixels into c (c->px malloc'd). NULL, or what is wrong with it. */
static const char *png_decode(const unsigned char *p, size_t n, Canvas *c) {
    static const unsigned char SIG[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    if (n < 8 || memcmp(p, SIG, 8)) return "not a PNG image";
    PngInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.key_r = pi.key_g = pi.key_b = -1;
    uint32_t w = 0, h = 0;
    int interlace = 0, have_header = 0;
    Out idat = {0};
    size_t at = 8;
    while (at + 12 <= n) {
        uint32_t len = be32(p + at);
        const unsigned char *type = p + at + 4, *body = p + at + 8;
        if (len > n - at - 12) break;  /* (cut short: what came before) */
        if (crc32_of(type, len + 4, 0) != be32(body + len)) {
            free(idat.data);
            return "a damaged PNG image (a chunk's checksum is wrong)";
        }
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            pi.depth = body[8];
            pi.color = body[9];
            interlace = body[12];
            have_header = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            pi.npalette = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < pi.npalette; i++)
                pi.palette[i] = 0xFF000000u | (uint32_t)body[3 * i] << 16 | (uint32_t)body[3 * i + 1] << 8 | body[3 * i + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (pi.color == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++) pi.palette[i] = (pi.palette[i] & 0xFFFFFF) | (uint32_t)body[i] << 24;
            } else if (pi.color == 0 && len >= 2) {
                pi.key_r = body[0] << 8 | body[1];
            } else if (pi.color == 2 && len >= 6) {
                pi.key_r = body[0] << 8 | body[1];
                pi.key_g = body[2] << 8 | body[3];
                pi.key_b = body[4] << 8 | body[5];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            for (uint32_t i = 0; i < len; i++) out_byte(&idat, body[i]);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    static const int channels[7] = {1, 0, 3, 1, 2, 0, 4};
    int ok_depth = pi.depth == 8 || pi.depth == 16 || ((pi.color == 0 || pi.color == 3) && (pi.depth == 1 || pi.depth == 2 || pi.depth == 4));
    if (!have_header || w == 0 || h == 0 || pi.color > 6 || !channels[pi.color] || !ok_depth || interlace > 1) {
        free(idat.data);
        return "not a PNG image this can read (its header)";
    }
    if ((uint64_t)w * h > MAX_PIXELS) {
        free(idat.data);
        return "too big (more than 64 million pixels)";
    }
    pi.channels = channels[pi.color];
    if (pi.depth < 8 && pi.color == 0 && pi.key_r >= 0) pi.key_r &= (1 << pi.depth) - 1;
    else if (pi.depth == 8 && pi.key_r >= 0) {  /* (8-bit samples are compared with the key as 8 bits) */
        pi.key_r &= 255;
        if (pi.key_g >= 0) pi.key_g &= 255;
        if (pi.key_b >= 0) pi.key_b &= 255;
    }
    int bits = pi.depth * pi.channels, bpp = bits / 8 > 0 ? bits / 8 : 1;
    /* the passes: Adam7 (interlaced), or the one whole image */
    static const int PX[7] = {0, 4, 0, 2, 0, 1, 0}, PY[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int DX[7] = {8, 8, 4, 4, 2, 2, 1}, DY[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = interlace ? 7 : 1;
    size_t total = 0;
    for (int k = 0; k < passes; k++) {
        uint32_t pw = interlace ? (w - (uint32_t)PX[k] + (uint32_t)DX[k] - 1) / (uint32_t)DX[k] : w;
        uint32_t ph = interlace ? (h - (uint32_t)PY[k] + (uint32_t)DY[k] - 1) / (uint32_t)DY[k] : h;
        if (interlace && (w <= (uint32_t)PX[k] || h <= (uint32_t)PY[k])) continue;
        total += (((size_t)pw * (size_t)bits + 7) / 8 + 1) * ph;
    }
    if (total / 1032 > idat.len) {  /* (deflate makes at most 1032 bytes from one: not before allocating it all) */
        free(idat.data);
        return "a damaged PNG image (its compressed pixels)";
    }
    unsigned char *raw = (unsigned char *)malloc(total ? total : 1);
    c->px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * h);
    if (!raw || !c->px) abort();  /* LCOV_EXCL_LINE: out of memory */
    c->w = (int)w;
    c->h = (int)h;
    const char *why = NULL;
    if (!zlib_inflate(idat.data, idat.len, raw, total)) {
        why = "a damaged PNG image (its compressed pixels)";
    } else {
        unsigned char *r = raw;
        for (int k = 0; k < passes && !why; k++) {
            if (interlace && (w <= (uint32_t)PX[k] || h <= (uint32_t)PY[k])) continue;
            int pw = interlace ? (int)((w - (uint32_t)PX[k] + (uint32_t)DX[k] - 1) / (uint32_t)DX[k]) : (int)w;
            int ph = interlace ? (int)((h - (uint32_t)PY[k] + (uint32_t)DY[k] - 1) / (uint32_t)DY[k]) : (int)h;
            size_t stride = ((size_t)pw * (size_t)bits + 7) / 8;
            if (!unfilter(r, ph, stride, bpp)) {
                why = "a damaged PNG image (a row's filter)";
                break;
            }
            for (int y = 0; y < ph; y++) {
                int oy = interlace ? PY[k] + y * DY[k] : y, ox = interlace ? PX[k] : 0, step = interlace ? DX[k] : 1;
                png_row(&pi, r + (size_t)y * (stride + 1) + 1, pw, c->px + (size_t)oy * w + (size_t)ox, step);
            }
            r += (stride + 1) * (size_t)ph;
        }
    }
    free(raw);
    free(idat.data);
    if (why) {
        free(c->px);
        c->px = NULL;
    }
    return why;
}

static void put_be32(Out *o, uint32_t v) {
    for (int k = 3; k >= 0; k--) out_byte(o, (unsigned char)(v >> (8 * k)));
}

static void put_chunk(Out *o, const char *type, const unsigned char *body, size_t len) {
    put_be32(o, (uint32_t)len);
    size_t start = o->len;
    for (int i = 0; i < 4; i++) out_byte(o, (unsigned char)type[i]);
    for (size_t i = 0; i < len; i++) out_byte(o, body[i]);
    put_be32(o, crc32_of(o->data + start, len + 4, 0));
}

/* c as a PNG (8-bit RGBA), into o. */
static void png_encode(const Canvas *c, Out *o) {
    static const unsigned char SIG[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    for (int i = 0; i < 8; i++) out_byte(o, SIG[i]);
    unsigned char ihdr[13] = {0};
    for (int k = 0; k < 4; k++) {
        ihdr[k] = (unsigned char)((uint32_t)c->w >> (24 - 8 * k));
        ihdr[4 + k] = (unsigned char)((uint32_t)c->h >> (24 - 8 * k));
    }
    ihdr[8] = 8;  /* bits */
    ihdr[9] = 6;  /* RGBA */
    put_chunk(o, "IHDR", ihdr, 13);
    size_t stride = (size_t)c->w * 4;
    unsigned char *rows = (unsigned char *)malloc((stride + 1) * (size_t)c->h), *rgba = (unsigned char *)malloc(stride * 2),
                  *trial = (unsigned char *)malloc(stride);
    if (!rows || !rgba || !trial) abort();  /* LCOV_EXCL_LINE: out of memory */
    unsigned char *cur = rgba, *prior = rgba + stride;
    memset(prior, 0, stride);
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            uint32_t v = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            cur[4 * x] = (unsigned char)(v >> 16);
            cur[4 * x + 1] = (unsigned char)(v >> 8);
            cur[4 * x + 2] = (unsigned char)v;
            cur[4 * x + 3] = (unsigned char)(v >> 24);
        }
        unsigned char *row = rows + (size_t)y * (stride + 1);
        long best = -1;
        for (int type = 0; type < 5; type++) {  /* the filter whose bytes look smallest */
            long sum = 0;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= 4 ? cur[i - 4] : 0, b = y ? prior[i] : 0, cc = y && i >= 4 ? prior[i - 4] : 0;
                int pred = type == 0 ? 0 : type == 1 ? a : type == 2 ? b : type == 3 ? (a + b) >> 1 : paeth(a, b, cc);
                trial[i] = (unsigned char)(cur[i] - pred);
                sum += trial[i] < 128 ? trial[i] : 256 - trial[i];
            }
            if (best < 0 || sum < best) {
                best = sum;
                row[0] = (unsigned char)type;
                memcpy(row + 1, trial, stride);
            }
        }
        unsigned char *t = prior;
        prior = cur;
        cur = t;
    }
    Out z = {0};
    zlib_deflate(rows, (stride + 1) * (size_t)c->h, &z);
    put_chunk(o, "IDAT", z.data, z.len);
    put_chunk(o, "IEND", NULL, 0);
    free(z.data);
    free(rows);
    free(rgba);
    free(trial);
}
