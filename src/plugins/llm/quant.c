/*
 * llm/quant.c - the tensors' number formats (ggml's): floats (F32, F16,
 * BF16), and blocks of small integers with a scale (Q4_0, Q4_1, Q5_0, Q5_1,
 * Q8_0, and the "K" formats Q4_K, Q5_K, Q6_K, in super-blocks of 256).
 *
 * Weights stay in their format. A product of a row of them with a vector
 * quantizes the vector first (int8 in blocks of 32, each with a float
 * scale) and multiplies integers: the integer sums are exact, and the
 * blocks' float results go to eight running sums (block k to sum k % 8),
 * added up in a fixed order, so every system gives the same bits. Float
 * weights multiply float vectors, in sixteen running sums (again in a
 * fixed order, with no fused multiply-adds). The products are
 * written three times: for NEON (ARM's 64-bit CPUs), for SSE2 (every
 * x86-64) and in plain C, each giving the others' results. (Part of llm.c.)
 */

enum {
    T_F32 = 0, T_F16 = 1, T_Q4_0 = 2, T_Q4_1 = 3, T_Q5_0 = 6, T_Q5_1 = 7, T_Q8_0 = 8, T_Q8_1 = 9, T_Q2_K = 10,
    T_Q3_K = 11, T_Q4_K = 12, T_Q5_K = 13, T_Q6_K = 14, T_Q8_K = 15, T_BF16 = 30, T_NTYPES = 40
};

static const struct {
    const char *name;
    int block, size;  /* how many numbers a block has, and its bytes */
    int usable;       /* 1: llm computes with it */
} TYPES[T_NTYPES] = {
    [0] = {"F32", 1, 4, 1}, [1] = {"F16", 1, 2, 1}, [2] = {"Q4_0", 32, 18, 1}, [3] = {"Q4_1", 32, 20, 1},
    [6] = {"Q5_0", 32, 22, 1}, [7] = {"Q5_1", 32, 24, 1}, [8] = {"Q8_0", 32, 34, 1}, [9] = {"Q8_1", 32, 36, 0},
    [10] = {"Q2_K", 256, 84, 0}, [11] = {"Q3_K", 256, 110, 0}, [12] = {"Q4_K", 256, 144, 1},
    [13] = {"Q5_K", 256, 176, 1}, [14] = {"Q6_K", 256, 210, 1}, [15] = {"Q8_K", 256, 292, 0},
    [16] = {"IQ2_XXS", 256, 66, 0}, [17] = {"IQ2_XS", 256, 74, 0}, [18] = {"IQ3_XXS", 256, 98, 0},
    [19] = {"IQ1_S", 256, 50, 0}, [20] = {"IQ4_NL", 32, 18, 0}, [21] = {"IQ3_S", 256, 110, 0},
    [22] = {"IQ2_S", 256, 82, 0}, [23] = {"IQ4_XS", 256, 136, 0}, [24] = {"I8", 1, 1, 0}, [25] = {"I16", 1, 2, 0},
    [26] = {"I32", 1, 4, 0}, [27] = {"I64", 1, 8, 0}, [28] = {"F64", 1, 8, 0}, [29] = {"IQ1_M", 256, 56, 0},
    [30] = {"BF16", 1, 2, 1}, [34] = {"TQ1_0", 256, 54, 0}, [35] = {"TQ2_0", 256, 66, 0},
    [39] = {"MXFP4", 32, 17, 0},
};

static int type_known(uint32_t t) { return t < T_NTYPES && TYPES[t].name; }
static const char *type_name(int t) { return TYPES[t].name; }
static int type_block(int t) { return TYPES[t].block; }
static int type_usable(int t) { return TYPES[t].usable; }
static int type_is_float(int t) { return t == T_F32 || t == T_F16 || t == T_BF16; }
static int64_t type_row_bytes(int t, int64_t n) { return n / TYPES[t].block * TYPES[t].size; }

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(LLM_NO_SIMD)
#define LLM_NEON 1
#include <arm_neon.h>
#elif defined(__SSE2__) && !defined(LLM_NO_SIMD)  /* (every x86-64) */
#define LLM_SSE2 1
#include <emmintrin.h>
#endif

/* ---------- halves ---------- */

#ifdef LLM_NEON
static float half(const unsigned char *p) {
    uint16_t h = (uint16_t)(p[0] | p[1] << 8);
    return vgetq_lane_f32(vcvt_f32_f16(vreinterpret_f16_u16(vdup_n_u16(h))), 0);
}
#else
static float half(const unsigned char *p) {
    unsigned h = (unsigned)p[0] | (unsigned)p[1] << 8;
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, man = h & 0x3ff, bits;
    if (e == 0) {  /* zero, or a number below the smallest normal one */
        float f = (float)man * (1.0f / 16777216.0f);
        return sign ? -f : f;
    }
    bits = e == 31 ? sign | 0x7f800000u | man << 13 : sign | (e + 112) << 23 | man << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
#endif

static float bf16(const unsigned char *p) {
    uint32_t bits = ((uint32_t)p[0] | (uint32_t)p[1] << 8) << 16;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ---------- a row as floats ---------- */

/* Q4_K's and Q5_K's eight 6-bit scales and mins, from their 12 bytes */
static void kscales(const unsigned char *s12, int *sc, int *mn) {
    uint32_t u0 = rd_u32(s12), u1 = rd_u32(s12 + 4), u2 = rd_u32(s12 + 8);
    uint32_t s[2] = {u0 & 0x3f3f3f3f, (u2 & 0x0f0f0f0f) | ((u0 >> 6) & 0x03030303) << 4};
    uint32_t m[2] = {u1 & 0x3f3f3f3f, ((u2 >> 4) & 0x0f0f0f0f) | ((u1 >> 6) & 0x03030303) << 4};
    for (int i = 0; i < 8; i++) {
        sc[i] = (int)(s[i / 4] >> (8 * (i % 4))) & 0xff;
        mn[i] = (int)(m[i / 4] >> (8 * (i % 4))) & 0xff;
    }
}

/* The n numbers of a row of type t at p, as floats. */
static void dequant_row(int t, const unsigned char *p, float *out, int64_t n) {
    int64_t nb = n / TYPES[t].block;
    for (int64_t b = 0; b < nb; b++, p += TYPES[t].size) {
        float *y = out + b * TYPES[t].block;
        if (t == T_F32) {
            memcpy(y, p, 4);
        } else if (t == T_F16) {
            y[0] = half(p);
        } else if (t == T_BF16) {
            y[0] = bf16(p);
        } else if (t == T_Q4_0 || t == T_Q4_1) {
            float d = half(p), m = t == T_Q4_1 ? half(p + 2) : 0;
            const unsigned char *qs = p + (t == T_Q4_1 ? 4 : 2);
            int off = t == T_Q4_1 ? 0 : 8;
            for (int j = 0; j < 16; j++) {
                y[j] = (float)((qs[j] & 15) - off) * d + m;
                y[j + 16] = (float)((qs[j] >> 4) - off) * d + m;
            }
        } else if (t == T_Q5_0 || t == T_Q5_1) {
            float d = half(p), m = t == T_Q5_1 ? half(p + 2) : 0;
            uint32_t qh = rd_u32(p + (t == T_Q5_1 ? 4 : 2));
            const unsigned char *qs = p + (t == T_Q5_1 ? 8 : 6);
            int off = t == T_Q5_1 ? 0 : 16;
            for (int j = 0; j < 16; j++) {
                y[j] = (float)(((qs[j] & 15) | (int)((qh >> j) & 1) << 4) - off) * d + m;
                y[j + 16] = (float)(((qs[j] >> 4) | (int)((qh >> (j + 16)) & 1) << 4) - off) * d + m;
            }
        } else if (t == T_Q8_0) {
            float d = half(p);
            for (int j = 0; j < 32; j++) y[j] = (float)(int8_t)p[2 + j] * d;
        } else if (t == T_Q4_K || t == T_Q5_K) {
            float d = half(p), dmin = half(p + 2);
            const unsigned char *qh = p + 16, *qs = p + (t == T_Q5_K ? 48 : 16);
            int sc[8], mn[8];
            kscales(p + 4, sc, mn);
            for (int j = 0; j < 4; j++, qs += 32) {
                float d0 = d * (float)sc[2 * j], d1 = d * (float)sc[2 * j + 1];
                float n0 = dmin * (float)mn[2 * j], n1 = dmin * (float)mn[2 * j + 1];
                for (int l = 0; l < 32; l++) {
                    int h0 = t == T_Q5_K ? ((qh[l] >> (2 * j)) & 1) << 4 : 0;
                    int h1 = t == T_Q5_K ? ((qh[l] >> (2 * j + 1)) & 1) << 4 : 0;
                    y[64 * j + l] = d0 * (float)((qs[l] & 15) | h0) - n0;
                    y[64 * j + 32 + l] = d1 * (float)((qs[l] >> 4) | h1) - n1;
                }
            }
        } else {  /* T_Q6_K */
            float d = half(p + 208);
            const unsigned char *ql = p, *qh = p + 128;
            const int8_t *sc = (const int8_t *)(p + 192);
            for (int h = 0; h < 2; h++, ql += 64, qh += 32, sc += 8) {
                for (int l = 0; l < 32; l++) {
                    int is = l / 16;
                    y[128 * h + l] = d * (float)sc[is] * (float)(((ql[l] & 15) | ((qh[l] & 3) << 4)) - 32);
                    y[128 * h + l + 32] = d * (float)sc[is + 2] * (float)(((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32);
                    y[128 * h + l + 64] = d * (float)sc[is + 4] * (float)(((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32);
                    y[128 * h + l + 96] = d * (float)sc[is + 6] * (float)(((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
                }
            }
        }
    }
}

/* ---------- vectors for the products ---------- */

/* A vector quantized for the products: int8 numbers, and for each block of 32 its scale and the sum of its
 * numbers. Its blocks are padded with zeros to a multiple of 4 (the products take them 4 at a time). */
typedef struct {
    int8_t *q;
    float *d;
    int32_t *s;
} Act;

static int64_t blocks4(int64_t n) { return (n / 32 + 3) / 4 * 4; }

/* x (n numbers, a multiple of 32) as int8 in blocks: scale amax / 127, rounded to the nearest (even) integer */
static void quantize_act(const float *x, int64_t n, Act a) {
    int64_t b = 0;
    for (; b < n / 32; b++, x += 32) {
        float amax = 0;
        for (int i = 0; i < 32; i++) amax = fabsf(x[i]) > amax ? fabsf(x[i]) : amax;
        float d = amax / 127.0f, id = d != 0 ? 1.0f / d : 0.0f;
        int32_t sum = 0;
        for (int i = 0; i < 32; i++) {
            int q = (int)lrintf(x[i] * id);
            a.q[32 * b + i] = (int8_t)q;
            sum += q;
        }
        a.d[b] = d;
        a.s[b] = sum;
    }
    for (; b < blocks4(n); b++) {
        memset(a.q + 32 * b, 0, 32);
        a.d[b] = 0;
        a.s[b] = 0;
    }
}

#define MAX_BATCH 32  /* tokens at a time through the forward pass (and the products) */

#ifndef LLM_NEON
static float sum16(float *acc) {
    for (int s = 8; s; s >>= 1)
        for (int j = 0; j < s; j++) acc[j] += acc[j + s];
    return acc[0];
}

/* eight running sums added up: (0 + 4) + (1 + 5) and (2 + 6) + (3 + 7), then (those two) */
static float sum8(const float *a) { return ((a[0] + a[4]) + (a[1] + a[5])) + ((a[2] + a[6]) + (a[3] + a[7])); }
#endif

#ifdef LLM_NEON
/* ======================================================================
 * NEON: the same sums as the plain C below, in the same order, by vectors
 * ==================================================================== */

/* the sixteen sums (four vectors) added up in sum16's order */
static float sum16_neon(const float32x4_t *a) {
    float32x4_t s = vaddq_f32(vaddq_f32(a[0], a[2]), vaddq_f32(a[1], a[3]));
    float32x2_t t = vadd_f32(vget_low_f32(s), vget_high_f32(s));
    return vget_lane_f32(t, 0) + vget_lane_f32(t, 1);
}

/* the four running sums (a vector) added up: (0 + 1) + (2 + 3) */
static float sum4f(float32x4_t a) {
    float32x2_t t = vpadd_f32(vget_low_f32(a), vget_high_f32(a));
    return vget_lane_f32(t, 0) + vget_lane_f32(t, 1);
}

static float32x4_t load4(int t, const unsigned char *p) {
    if (t == T_F32) return vld1q_f32((const float *)(const void *)p);
    if (t == T_F16) return vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16((const uint16_t *)(const void *)p)));
    return vreinterpretq_f32_u32(vshll_n_u16(vld1_u16((const uint16_t *)(const void *)p), 16));
}

/* out[b] = w . x_b for B vectors (xs floats apart), n numbers: lane i % 16 of sixteen sums takes number i */
static void dot_float(int t, const unsigned char *w, int64_t n, const float *x, int64_t xs, int B, float *out) {
    float32x4_t r[4], acc[MAX_BATCH][4], wv[4];
    int size = TYPES[t].size;
    int64_t i = 0, whole = n / 16 * 16;
    for (int j = 0; j < 4; j++) r[j] = vdupq_n_f32(0);
    if (B == 1) {
        for (; i < whole; i += 16)
            for (int j = 0; j < 4; j++) r[j] = vaddq_f32(r[j], vmulq_f32(load4(t, w + (i + 4 * j) * size), vld1q_f32(x + i + 4 * j)));
    } else {
        for (int b = 0; b < B; b++)
            for (int j = 0; j < 4; j++) acc[b][j] = r[j];
        for (; i < whole; i += 16) {
            for (int j = 0; j < 4; j++) wv[j] = load4(t, w + (i + 4 * j) * size);
            for (int b = 0; b < B; b++)
                for (int j = 0; j < 4; j++) acc[b][j] = vaddq_f32(acc[b][j], vmulq_f32(wv[j], vld1q_f32(x + b * xs + i + 4 * j)));
        }
    }
    float wt[16] = {0}, xt[16] = {0};
    if (i < n) dequant_row(t, w + i * size, wt, n - i);  /* (the end: padded with zeros) */
    for (int b = 0; b < B; b++) {
        float32x4_t *v = B == 1 ? r : acc[b];
        if (i < n) {
            memcpy(xt, x + b * xs + i, sizeof(float) * (size_t)(n - i));
            for (int j = 0; j < 4; j++) v[j] = vaddq_f32(v[j], vmulq_f32(vld1q_f32(wt + 4 * j), vld1q_f32(xt + 4 * j)));
        }
        out[b] = sum16_neon(v);
    }
}

static int32x4_t dot16(int8x16_t w, const int8_t *a) {
#ifdef __ARM_FEATURE_DOTPROD
    return vdotq_s32(vdupq_n_s32(0), w, vld1q_s8(a));
#else
    int8x16_t x = vld1q_s8(a);
    return vpadalq_s16(vpaddlq_s16(vmull_s8(vget_low_s8(w), vget_low_s8(x))), vmull_s8(vget_high_s8(w), vget_high_s8(x)));
#endif
}

static int32x4_t dot32(int8x16_t w0, int8x16_t w1, const int8_t *a) {
#ifdef __ARM_FEATURE_DOTPROD
    return vdotq_s32(vdotq_s32(vdupq_n_s32(0), w0, vld1q_s8(a)), w1, vld1q_s8(a + 16));
#else
    return vaddq_s32(dot16(w0, a), dot16(w1, a + 16));
#endif
}

/* four blocks' partial sums (a vector each) as one vector of their totals */
static int32x4_t sum4(int32x4_t a, int32x4_t b, int32x4_t c, int32x4_t d) {
    return vpaddq_s32(vpaddq_s32(a, b), vpaddq_s32(c, d));
}

/* 16 fifth bits (0 or 16) for 16 numbers, from 16 bits of qh starting at bit `from` */
static uint8x16_t high_bits(uint32_t qh, int from) {
    const uint8x16_t bit = vreinterpretq_u8_u64(vdupq_n_u64(0x8040201008040201ull));
    uint8x16_t b = vcombine_u8(vdup_n_u8((uint8_t)(qh >> from)), vdup_n_u8((uint8_t)(qh >> (from + 8))));
    return vandq_u8(vtstq_u8(b, bit), vdupq_n_u8(16));
}

#define S8(u) vreinterpretq_s8_u8(u)

/* the 32 numbers of a block of Q4_0, Q4_1, Q5_0, Q5_1 or Q8_0 at p as int8 (lo: the first 16) */
static void unpack32(int t, const unsigned char *p, int8x16_t *lo, int8x16_t *hi) {
    if (t == T_Q8_0) {
        *lo = vld1q_s8((const int8_t *)p + 2);
        *hi = vld1q_s8((const int8_t *)p + 18);
        return;
    }
    int at = t == T_Q4_0 ? 2 : t == T_Q4_1 ? 4 : t == T_Q5_0 ? 6 : 8, off = t == T_Q4_0 ? 8 : t == T_Q5_0 ? 16 : 0;
    uint8x16_t q = vld1q_u8(p + at), l = vandq_u8(q, vdupq_n_u8(15)), h = vshrq_n_u8(q, 4);
    if (t == T_Q5_0 || t == T_Q5_1) {
        uint32_t qh = rd_u32(p + at - 4);
        l = vorrq_u8(l, high_bits(qh, 0));
        h = vorrq_u8(h, high_bits(qh, 16));
    }
    *lo = vsubq_s8(S8(l), vdupq_n_s8((int8_t)off));
    *hi = vsubq_s8(S8(h), vdupq_n_s8((int8_t)off));
}

/* The blocks' results go to eight running sums (block k to sum k % 8: two vectors, for groups of four blocks
 * in turn), added up at the end as (0 + 4) + (1 + 5) and (2 + 6) + (3 + 7), then those four as (0 + 1) + (2 + 3). */
typedef struct {
    float32x4_t one[2];                /* for B = 1, kept in registers */
    float32x4_t many[MAX_BATCH][2];
    int B;
} Sums;

static void sums_start(Sums *s, int B) {
    s->B = B;
    s->one[0] = s->one[1] = vdupq_n_f32(0);
    for (int b = 0; b < B; b++) s->many[b][0] = s->many[b][1] = vdupq_n_f32(0);
}

static void sums_end(const Sums *s, float *out) {
    for (int b = 0; b < s->B; b++) {
        const float32x4_t *v = s->B == 1 ? s->one : s->many[b];
        out[b] = sum4f(vaddq_f32(v[0], v[1]));
    }
}

/* the 4 blocks of 32 from g on: their numbers as int8, and scales */
typedef struct {
    int8x16_t w[8];
    float32x4_t d, m;
} Group32;

static void group32(int t, const unsigned char *row, int64_t g, int64_t nb, Group32 *G) {
    unsigned char pad[4 * 34];
    int size = TYPES[t].size;
    const unsigned char *p = row + g * size;
    if (g + 4 > nb) {  /* (the end: padded with blocks of zeros, which the vector's zeros multiply) */
        memset(pad, 0, sizeof pad);
        memcpy(pad, p, (size_t)((nb - g) * size));
        p = pad;
    }
    uint16x4_t dh = vdup_n_u16(0), mh = vdup_n_u16(0);
    for (int k = 0; k < 4; k++) unpack32(t, p + k * size, &G->w[2 * k], &G->w[2 * k + 1]);
    dh = vld1_lane_u16((const uint16_t *)(const void *)p, dh, 0);
    dh = vld1_lane_u16((const uint16_t *)(const void *)(p + size), dh, 1);
    dh = vld1_lane_u16((const uint16_t *)(const void *)(p + 2 * size), dh, 2);
    dh = vld1_lane_u16((const uint16_t *)(const void *)(p + 3 * size), dh, 3);
    if (t == T_Q4_1 || t == T_Q5_1) {
        mh = vld1_lane_u16((const uint16_t *)(const void *)(p + 2), mh, 0);
        mh = vld1_lane_u16((const uint16_t *)(const void *)(p + size + 2), mh, 1);
        mh = vld1_lane_u16((const uint16_t *)(const void *)(p + 2 * size + 2), mh, 2);
        mh = vld1_lane_u16((const uint16_t *)(const void *)(p + 3 * size + 2), mh, 3);
    }
    G->d = vcvt_f32_f16(vreinterpret_f16_u16(dh));
    G->m = vcvt_f32_f16(vreinterpret_f16_u16(mh));
}

/* the 4 blocks' results with the vector's blocks (q, d, s at the first) */
static float32x4_t part32(const Group32 *G, int has_min, const int8_t *q, const float *d, const int32_t *s) {
    int32x4_t is = sum4(dot32(G->w[0], G->w[1], q), dot32(G->w[2], G->w[3], q + 32), dot32(G->w[4], G->w[5], q + 64),
                        dot32(G->w[6], G->w[7], q + 96));
    float32x4_t da = vld1q_f32(d), c = vmulq_f32(vmulq_f32(G->d, da), vcvtq_f32_s32(is));
    return has_min ? vaddq_f32(c, vmulq_f32(vmulq_f32(G->m, da), vcvtq_f32_s32(vld1q_s32(s)))) : c;
}

/* blocks of 32 (Q4_0, Q4_1, Q5_0, Q5_1, Q8_0), four at a time */
static void dot_q32(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    Sums S;
    Group32 G;
    int has_min = t == T_Q4_1 || t == T_Q5_1;
    sums_start(&S, B);
    for (int64_t g = 0; g < nb; g += 4) {
        int par = (int)(g >> 2) & 1;
        group32(t, row, g, nb, &G);
        if (B == 1) {
            float32x4_t c = part32(&G, has_min, a.q + 32 * g, a.d + g, a.s + g);
            if (par) S.one[1] = vaddq_f32(S.one[1], c);
            else S.one[0] = vaddq_f32(S.one[0], c);
        } else {
            for (int b = 0; b < B; b++)
                S.many[b][par] = vaddq_f32(S.many[b][par], part32(&G, has_min, a.q + b * qs + 32 * g, a.d + b * ds + g, a.s + b * ds + g));
        }
    }
    sums_end(&S, out);
}

/* the 4 sub-blocks h * 4.. of a Q4_K/Q5_K super-block: their numbers, integer scales and mins */
typedef struct {
    int8x16_t w[8];
    int32x4_t sc, mn;
} GroupK;

static void groupk(int t, const unsigned char *row, int h, int32x4_t sc, int32x4_t mn, GroupK *G) {
    const unsigned char *qh = row + 16, *qsb = row + (t == T_Q5_K ? 48 : 16);
    for (int j = 0; j < 2; j++) {  /* pair 2h + j: its low nibbles are sub-block 4h + 2j, its high ones the next */
        int pair = 2 * h + j;
        uint8x16_t q0 = vld1q_u8(qsb + 32 * pair), q1 = vld1q_u8(qsb + 32 * pair + 16), m = vdupq_n_u8(15);
        uint8x16_t l0 = vandq_u8(q0, m), l1 = vandq_u8(q1, m), h0 = vshrq_n_u8(q0, 4), h1 = vshrq_n_u8(q1, 4);
        if (t == T_Q5_K) {
            uint8x16_t b0 = vld1q_u8(qh), b1 = vld1q_u8(qh + 16), sixteen = vdupq_n_u8(16);
            uint8x16_t bl = vdupq_n_u8((uint8_t)(1 << (2 * pair))), bh = vdupq_n_u8((uint8_t)(2 << (2 * pair)));
            l0 = vorrq_u8(l0, vandq_u8(vtstq_u8(b0, bl), sixteen));
            l1 = vorrq_u8(l1, vandq_u8(vtstq_u8(b1, bl), sixteen));
            h0 = vorrq_u8(h0, vandq_u8(vtstq_u8(b0, bh), sixteen));
            h1 = vorrq_u8(h1, vandq_u8(vtstq_u8(b1, bh), sixteen));
        }
        G->w[4 * j] = S8(l0);
        G->w[4 * j + 1] = S8(l1);
        G->w[4 * j + 2] = S8(h0);
        G->w[4 * j + 3] = S8(h1);
    }
    G->sc = sc;
    G->mn = mn;
}

static float32x4_t partk(const GroupK *G, float d, float dmin, const int8_t *q, const float *dd, const int32_t *s) {
    int32x4_t is = sum4(dot32(G->w[0], G->w[1], q), dot32(G->w[2], G->w[3], q + 32), dot32(G->w[4], G->w[5], q + 64),
                        dot32(G->w[6], G->w[7], q + 96));
    float32x4_t da = vld1q_f32(dd);
    float32x4_t x = vmulq_f32(vmulq_n_f32(da, d), vcvtq_f32_s32(vmulq_s32(G->sc, is)));
    return vsubq_f32(x, vmulq_f32(vmulq_n_f32(da, dmin), vcvtq_f32_s32(vmulq_s32(G->mn, vld1q_s32(s)))));
}

/* Q4_K and Q5_K: super-blocks of 8 sub-blocks of 32, each with a 6-bit scale and min; 4 sub-blocks at a time */
static void dot_k45(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    Sums S;
    GroupK G;
    sums_start(&S, B);
    for (int64_t k = 0; k < nb; k += 8, row += TYPES[t].size) {
        float d = half(row), dmin = half(row + 2);
        uint32_t u0 = rd_u32(row + 4), u1 = rd_u32(row + 8), u2 = rd_u32(row + 12);  /* (kscales', in vectors) */
        uint64_t sc8 = (uint64_t)(u0 & 0x3f3f3f3f) | (uint64_t)((u2 & 0x0f0f0f0f) | ((u0 >> 6) & 0x03030303) << 4) << 32;
        uint64_t mn8 = (uint64_t)(u1 & 0x3f3f3f3f) | (uint64_t)(((u2 >> 4) & 0x0f0f0f0f) | ((u1 >> 6) & 0x03030303) << 4) << 32;
        uint16x8_t sc16 = vmovl_u8(vcreate_u8(sc8)), mn16 = vmovl_u8(vcreate_u8(mn8));
        for (int h = 0; h < 2; h++) {
            groupk(t, row, h, vreinterpretq_s32_u32(vmovl_u16(h ? vget_high_u16(sc16) : vget_low_u16(sc16))),
                   vreinterpretq_s32_u32(vmovl_u16(h ? vget_high_u16(mn16) : vget_low_u16(mn16))), &G);
            int64_t at = k + 4 * h;
            if (B == 1) {
                float32x4_t c = partk(&G, d, dmin, a.q + 32 * at, a.d + at, a.s + at);
                if (h) S.one[1] = vaddq_f32(S.one[1], c);
                else S.one[0] = vaddq_f32(S.one[0], c);
            } else {
                for (int b = 0; b < B; b++)
                    S.many[b][h] = vaddq_f32(S.many[b][h], partk(&G, d, dmin, a.q + b * qs + 32 * at, a.d + b * ds + at, a.s + b * ds + at));
            }
        }
    }
    sums_end(&S, out);
}

/* the 4 blocks of 32 of half h of a Q6_K super-block, as int8 */
static void group6(const unsigned char *row, int h, int8x16_t *w) {
    const unsigned char *ql = row + 64 * h, *qh = row + 128 + 32 * h;
    uint8x16_t h0 = vld1q_u8(qh), h1 = vld1q_u8(qh + 16), m = vdupq_n_u8(15), three = vdupq_n_u8(3);
    uint8x16_t l[4] = {vld1q_u8(ql), vld1q_u8(ql + 16), vld1q_u8(ql + 32), vld1q_u8(ql + 48)};
    int8x16_t thirty2 = vdupq_n_s8(32);
    for (int t = 0; t < 4; t++) {  /* block t: low or high nibbles of ql (its first or second 32), 2 bits of qh */
        uint8x16_t a0 = l[2 * (t & 1)], a1 = l[2 * (t & 1) + 1];
        a0 = t >= 2 ? vshrq_n_u8(a0, 4) : vandq_u8(a0, m);
        a1 = t >= 2 ? vshrq_n_u8(a1, 4) : vandq_u8(a1, m);
        int8x16_t sh = vdupq_n_s8((int8_t)(-2 * t));
        uint8x16_t b0 = vshlq_n_u8(vandq_u8(vshlq_u8(h0, sh), three), 4);
        uint8x16_t b1 = vshlq_n_u8(vandq_u8(vshlq_u8(h1, sh), three), 4);
        w[2 * t] = vsubq_s8(S8(vorrq_u8(a0, b0)), thirty2);
        w[2 * t + 1] = vsubq_s8(S8(vorrq_u8(a1, b1)), thirty2);
    }
}

static float32x4_t part6(const int8x16_t *w, const int8_t *sc, float d, const int8_t *q, const float *dd) {
    int32x4_t c[4];
    for (int t = 0; t < 4; t++)
        c[t] = vaddq_s32(vmulq_n_s32(dot16(w[2 * t], q + 32 * t), sc[2 * t]),
                         vmulq_n_s32(dot16(w[2 * t + 1], q + 32 * t + 16), sc[2 * t + 1]));
    return vmulq_f32(vmulq_n_f32(vld1q_f32(dd), d), vcvtq_f32_s32(sum4(c[0], c[1], c[2], c[3])));
}

/* Q6_K: super-blocks of 16 sub-blocks of 16, each with an int8 scale; blocks of 32 four at a time */
static void dot_q6k(const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    Sums S;
    int8x16_t w[8];
    sums_start(&S, B);
    for (int64_t k = 0; k < nb; k += 8, row += 210) {
        float d = half(row + 208);
        for (int h = 0; h < 2; h++) {
            const int8_t *sc = (const int8_t *)(row + 192) + 8 * h;
            int64_t at = k + 4 * h;
            group6(row, h, w);
            if (B == 1) {
                float32x4_t c = part6(w, sc, d, a.q + 32 * at, a.d + at);
                if (h) S.one[1] = vaddq_f32(S.one[1], c);
                else S.one[0] = vaddq_f32(S.one[0], c);
            } else {
                for (int b = 0; b < B; b++) S.many[b][h] = vaddq_f32(S.many[b][h], part6(w, sc, d, a.q + b * qs + 32 * at, a.d + b * ds + at));
            }
        }
    }
    sums_end(&S, out);
}

#elif defined(LLM_SSE2)
/* ======================================================================
 * SSE2 (every x86-64): the same sums as the plain C below, in the same
 * order, by vectors. (Integer products of scales are made in floats here,
 * which is exact: they stay below 2^24.)
 * ==================================================================== */

static float sum16_sse(const __m128 *a) {
    float t[16];
    for (int j = 0; j < 4; j++) _mm_storeu_ps(t + 4 * j, a[j]);
    return sum16(t);
}

static float sum8_sse(const __m128 *a) {
    float t[8];
    _mm_storeu_ps(t, a[0]);
    _mm_storeu_ps(t + 4, a[1]);
    return sum8(t);
}

static __m128 load4(int t, const unsigned char *p) {
    __m128i h = _mm_loadl_epi64((const __m128i *)(const void *)p);
    if (t == T_F32) return _mm_loadu_ps((const float *)(const void *)p);
    if (t == T_BF16) return _mm_castsi128_ps(_mm_unpacklo_epi16(_mm_setzero_si128(), h));
    /* halves: their exponent and mantissa moved into a float's, times 2^112 (exact, below normal numbers too) */
    h = _mm_unpacklo_epi16(h, _mm_setzero_si128());
    __m128i em = _mm_and_si128(h, _mm_set1_epi32(0x7fff)), sign = _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x8000)), 16);
    __m128 f = _mm_mul_ps(_mm_castsi128_ps(_mm_slli_epi32(em, 13)), _mm_set1_ps(5.192296858534828e33f));
    __m128i inf = _mm_and_si128(_mm_cmpgt_epi32(em, _mm_set1_epi32(0x7bff)), _mm_set1_epi32(0x7f800000));
    return _mm_or_ps(_mm_or_ps(f, _mm_castsi128_ps(inf)), _mm_castsi128_ps(sign));
}

static void dot_float(int t, const unsigned char *w, int64_t n, const float *x, int64_t xs, int B, float *out) {
    __m128 r[4], acc[MAX_BATCH][4], wv[4];
    int size = TYPES[t].size;
    int64_t i = 0, whole = n / 16 * 16;
    for (int j = 0; j < 4; j++) r[j] = _mm_setzero_ps();
    if (B == 1) {
        for (; i < whole; i += 16)
            for (int j = 0; j < 4; j++) r[j] = _mm_add_ps(r[j], _mm_mul_ps(load4(t, w + (i + 4 * j) * size), _mm_loadu_ps(x + i + 4 * j)));
    } else {
        for (int b = 0; b < B; b++)
            for (int j = 0; j < 4; j++) acc[b][j] = r[j];
        for (; i < whole; i += 16) {
            for (int j = 0; j < 4; j++) wv[j] = load4(t, w + (i + 4 * j) * size);
            for (int b = 0; b < B; b++)
                for (int j = 0; j < 4; j++) acc[b][j] = _mm_add_ps(acc[b][j], _mm_mul_ps(wv[j], _mm_loadu_ps(x + b * xs + i + 4 * j)));
        }
    }
    float wt[16] = {0}, xt[16] = {0};
    if (i < n) dequant_row(t, w + i * size, wt, n - i);
    for (int b = 0; b < B; b++) {
        __m128 *v = B == 1 ? r : acc[b];
        if (i < n) {
            memcpy(xt, x + b * xs + i, sizeof(float) * (size_t)(n - i));
            for (int j = 0; j < 4; j++) v[j] = _mm_add_ps(v[j], _mm_mul_ps(_mm_loadu_ps(wt + 4 * j), _mm_loadu_ps(xt + 4 * j)));
        }
        out[b] = sum16_sse(v);
    }
}

static __m128i ld16(const void *p) { return _mm_loadu_si128((const __m128i *)p); }

/* 16 int8 products, as four partial sums */
static __m128i dot16(__m128i w, __m128i a) {
    __m128i z = _mm_setzero_si128(), ws = _mm_cmpgt_epi8(z, w), as = _mm_cmpgt_epi8(z, a);
    return _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(w, ws), _mm_unpacklo_epi8(a, as)),
                         _mm_madd_epi16(_mm_unpackhi_epi8(w, ws), _mm_unpackhi_epi8(a, as)));
}

static __m128i dot32(__m128i w0, __m128i w1, const int8_t *a) { return _mm_add_epi32(dot16(w0, ld16(a)), dot16(w1, ld16(a + 16))); }

/* four blocks' partial sums (a vector each) as one vector of their totals */
static __m128i sum4(__m128i a, __m128i b, __m128i c, __m128i d) {
    __m128i u0 = _mm_add_epi32(_mm_unpacklo_epi32(a, b), _mm_unpackhi_epi32(a, b));
    __m128i u1 = _mm_add_epi32(_mm_unpacklo_epi32(c, d), _mm_unpackhi_epi32(c, d));
    return _mm_add_epi32(_mm_unpacklo_epi64(u0, u1), _mm_unpackhi_epi64(u0, u1));
}

/* 16 fifth bits (0 or 16) for 16 numbers, from 16 bits of qh starting at bit `from` */
static __m128i high_bits(uint32_t qh, int from) {
    char a = (char)(qh >> from), b = (char)(qh >> (from + 8));
    const __m128i bit = _mm_set_epi8(-128, 64, 32, 16, 8, 4, 2, 1, -128, 64, 32, 16, 8, 4, 2, 1);
    __m128i v = _mm_set_epi8(b, b, b, b, b, b, b, b, a, a, a, a, a, a, a, a);
    return _mm_and_si128(_mm_cmpeq_epi8(_mm_and_si128(v, bit), bit), _mm_set1_epi8(16));
}

/* the low and high nibbles of 16 bytes */
static __m128i lo4(__m128i q) { return _mm_and_si128(q, _mm_set1_epi8(15)); }
static __m128i hi4(__m128i q) { return _mm_and_si128(_mm_srli_epi16(q, 4), _mm_set1_epi8(15)); }

static void unpack32(int t, const unsigned char *p, __m128i *lo, __m128i *hi) {
    if (t == T_Q8_0) {
        *lo = ld16(p + 2);
        *hi = ld16(p + 18);
        return;
    }
    int at = t == T_Q4_0 ? 2 : t == T_Q4_1 ? 4 : t == T_Q5_0 ? 6 : 8, off = t == T_Q4_0 ? 8 : t == T_Q5_0 ? 16 : 0;
    __m128i q = ld16(p + at), l = lo4(q), h = hi4(q);
    if (t == T_Q5_0 || t == T_Q5_1) {
        uint32_t qh = rd_u32(p + at - 4);
        l = _mm_or_si128(l, high_bits(qh, 0));
        h = _mm_or_si128(h, high_bits(qh, 16));
    }
    *lo = _mm_sub_epi8(l, _mm_set1_epi8((char)off));
    *hi = _mm_sub_epi8(h, _mm_set1_epi8((char)off));
}

static void dot_q32(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    __m128 acc[MAX_BATCH][2];
    unsigned char pad[4 * 34];
    int size = TYPES[t].size, has_min = t == T_Q4_1 || t == T_Q5_1;
    for (int b = 0; b < B; b++) acc[b][0] = acc[b][1] = _mm_setzero_ps();
    for (int64_t g = 0; g < nb; g += 4) {
        const unsigned char *p = row + g * size;
        int par = (int)(g >> 2) & 1;
        if (g + 4 > nb) {  /* (the end: padded with blocks of zeros, which the vector's zeros multiply) */
            memset(pad, 0, sizeof pad);
            memcpy(pad, p, (size_t)((nb - g) * size));
            p = pad;
        }
        __m128i w[8];
        for (int k = 0; k < 4; k++) unpack32(t, p + k * size, &w[2 * k], &w[2 * k + 1]);
        __m128 dv = _mm_set_ps(half(p + 3 * size), half(p + 2 * size), half(p + size), half(p));
        __m128 mv = has_min ? _mm_set_ps(half(p + 3 * size + 2), half(p + 2 * size + 2), half(p + size + 2), half(p + 2)) : dv;
        for (int b = 0; b < B; b++) {
            const int8_t *q = a.q + b * qs + 32 * g;
            __m128i is = sum4(dot32(w[0], w[1], q), dot32(w[2], w[3], q + 32), dot32(w[4], w[5], q + 64), dot32(w[6], w[7], q + 96));
            __m128 da = _mm_loadu_ps(a.d + b * ds + g), c = _mm_mul_ps(_mm_mul_ps(dv, da), _mm_cvtepi32_ps(is));
            if (has_min) c = _mm_add_ps(c, _mm_mul_ps(_mm_mul_ps(mv, da), _mm_cvtepi32_ps(ld16(a.s + b * ds + g))));
            acc[b][par] = _mm_add_ps(acc[b][par], c);
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8_sse(acc[b]);
}

static void dot_k45(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    __m128 acc[MAX_BATCH][2];
    for (int b = 0; b < B; b++) acc[b][0] = acc[b][1] = _mm_setzero_ps();
    for (int64_t k = 0; k < nb; k += 8, row += TYPES[t].size) {
        float d = half(row), dmin = half(row + 2);
        int sc[8], mn[8];
        kscales(row + 4, sc, mn);
        const unsigned char *qh = row + 16, *qsb = row + (t == T_Q5_K ? 48 : 16);
        __m128i b0 = ld16(qh), b1 = ld16(qh + 16), one = _mm_set1_epi8(1);
        for (int h = 0; h < 2; h++) {
            __m128i w[8];
            for (int j = 0; j < 2; j++) {
                int pair = 2 * h + j;
                __m128i q0 = ld16(qsb + 32 * pair), q1 = ld16(qsb + 32 * pair + 16);
                __m128i l0 = lo4(q0), l1 = lo4(q1), h0 = hi4(q0), h1 = hi4(q1);
                if (t == T_Q5_K) {  /* (bit 2 pair of qh: the low half's fifth bit, the next one the high half's) */
                    __m128i sl = _mm_cvtsi32_si128(2 * pair), sh = _mm_cvtsi32_si128(2 * pair + 1);
                    l0 = _mm_or_si128(l0, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(b0, sl), one), 4));
                    l1 = _mm_or_si128(l1, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(b1, sl), one), 4));
                    h0 = _mm_or_si128(h0, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(b0, sh), one), 4));
                    h1 = _mm_or_si128(h1, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(b1, sh), one), 4));
                }
                w[4 * j] = l0;
                w[4 * j + 1] = l1;
                w[4 * j + 2] = h0;
                w[4 * j + 3] = h1;
            }
            __m128 scv = _mm_set_ps((float)sc[4 * h + 3], (float)sc[4 * h + 2], (float)sc[4 * h + 1], (float)sc[4 * h]);
            __m128 mnv = _mm_set_ps((float)mn[4 * h + 3], (float)mn[4 * h + 2], (float)mn[4 * h + 1], (float)mn[4 * h]);
            int64_t at = k + 4 * h;
            for (int b = 0; b < B; b++) {
                const int8_t *q = a.q + b * qs + 32 * at;
                __m128i is = sum4(dot32(w[0], w[1], q), dot32(w[2], w[3], q + 32), dot32(w[4], w[5], q + 64), dot32(w[6], w[7], q + 96));
                __m128 da = _mm_loadu_ps(a.d + b * ds + at);
                __m128 x = _mm_mul_ps(_mm_mul_ps(da, _mm_set1_ps(d)), _mm_mul_ps(scv, _mm_cvtepi32_ps(is)));
                __m128 y = _mm_mul_ps(_mm_mul_ps(da, _mm_set1_ps(dmin)), _mm_mul_ps(mnv, _mm_cvtepi32_ps(ld16(a.s + b * ds + at))));
                acc[b][h] = _mm_add_ps(acc[b][h], _mm_sub_ps(x, y));
            }
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8_sse(acc[b]);
}

static void dot_q6k(const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    __m128 acc[MAX_BATCH][2];
    for (int b = 0; b < B; b++) acc[b][0] = acc[b][1] = _mm_setzero_ps();
    for (int64_t k = 0; k < nb; k += 8, row += 210) {
        float d = half(row + 208);
        for (int h = 0; h < 2; h++) {
            const unsigned char *ql = row + 64 * h, *qh = row + 128 + 32 * h;
            const int8_t *sc = (const int8_t *)(row + 192) + 8 * h;
            __m128i h0 = ld16(qh), h1 = ld16(qh + 16), three = _mm_set1_epi8(3), thirty2 = _mm_set1_epi8(32);
            __m128i l[4] = {ld16(ql), ld16(ql + 16), ld16(ql + 32), ld16(ql + 48)}, w[8];
            for (int t = 0; t < 4; t++) {
                __m128i a0 = t >= 2 ? hi4(l[2 * (t & 1)]) : lo4(l[2 * (t & 1)]);
                __m128i a1 = t >= 2 ? hi4(l[2 * (t & 1) + 1]) : lo4(l[2 * (t & 1) + 1]);
                __m128i sh = _mm_cvtsi32_si128(2 * t);
                w[2 * t] = _mm_sub_epi8(_mm_or_si128(a0, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(h0, sh), three), 4)), thirty2);
                w[2 * t + 1] = _mm_sub_epi8(_mm_or_si128(a1, _mm_slli_epi16(_mm_and_si128(_mm_srl_epi16(h1, sh), three), 4)), thirty2);
            }
            int64_t at = k + 4 * h;
            __m128 sa = _mm_set_ps((float)sc[6], (float)sc[4], (float)sc[2], (float)sc[0]);
            __m128 sb = _mm_set_ps((float)sc[7], (float)sc[5], (float)sc[3], (float)sc[1]);
            for (int b = 0; b < B; b++) {
                const int8_t *q = a.q + b * qs + 32 * at;
                __m128i lo = sum4(dot16(w[0], ld16(q)), dot16(w[2], ld16(q + 32)), dot16(w[4], ld16(q + 64)), dot16(w[6], ld16(q + 96)));
                __m128i hi = sum4(dot16(w[1], ld16(q + 16)), dot16(w[3], ld16(q + 48)), dot16(w[5], ld16(q + 80)), dot16(w[7], ld16(q + 112)));
                __m128 v = _mm_add_ps(_mm_mul_ps(sa, _mm_cvtepi32_ps(lo)), _mm_mul_ps(sb, _mm_cvtepi32_ps(hi)));
                acc[b][h] = _mm_add_ps(acc[b][h], _mm_mul_ps(_mm_mul_ps(_mm_loadu_ps(a.d + b * ds + at), _mm_set1_ps(d)), v));
            }
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8_sse(acc[b]);
}

#else
/* ======================================================================
 * Plain C: sixteen running sums for floats; for blocks, exact integer
 * sums and eight running sums (block k to sum k % 8)
 * ==================================================================== */

static float one_float(int t, const unsigned char *p) {
    float f;
    if (t == T_F16) return half(p);
    if (t == T_BF16) return bf16(p);
    memcpy(&f, p, 4);
    return f;
}

static void dot_float(int t, const unsigned char *w, int64_t n, const float *x, int64_t xs, int B, float *out) {
    int size = TYPES[t].size;
    for (int b = 0; b < B; b++) {
        float acc[16] = {0};
        for (int64_t i = 0; i < n; i++) acc[i & 15] += one_float(t, w + i * size) * x[b * xs + i];
        out[b] = sum16(acc);
    }
}

/* n int8 products (loops a compiler makes vector code of) */
static int32_t dot_i8(const int8_t *w, const int8_t *x, int n) {
    int32_t s = 0;
    for (int i = 0; i < n; i++) s += w[i] * x[i];
    return s;
}

/* the 32 numbers of a block of Q4_0, Q4_1, Q5_0 or Q5_1 at p as int8 (Q8_0's are there already) */
static void unpack32(int t, const unsigned char *p, int8_t *w) {
    int at = t == T_Q4_0 ? 2 : t == T_Q4_1 ? 4 : t == T_Q5_0 ? 6 : 8, off = t == T_Q4_0 ? 8 : t == T_Q5_0 ? 16 : 0;
    uint32_t qh = t == T_Q5_0 || t == T_Q5_1 ? rd_u32(p + at - 4) : 0;
    for (int j = 0; j < 16; j++) {
        w[j] = (int8_t)(((p[at + j] & 15) | (int)((qh >> j) & 1) << 4) - off);
        w[j + 16] = (int8_t)(((p[at + j] >> 4) | (int)((qh >> (j + 16)) & 1) << 4) - off);
    }
}

static void dot_q32(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    int size = TYPES[t].size, has_min = t == T_Q4_1 || t == T_Q5_1;
    float acc[MAX_BATCH][8];
    int8_t w[32];
    memset(acc, 0, sizeof acc);
    for (int64_t k = 0; k < nb; k++) {
        const unsigned char *p = row + k * size;
        const int8_t *wk = (const int8_t *)p + 2;
        if (t != T_Q8_0) {
            unpack32(t, p, w);
            wk = w;
        }
        for (int b = 0; b < B; b++) {
            float da = a.d[b * ds + k];
            float s = (half(p) * da) * (float)dot_i8(wk, a.q + b * qs + 32 * k, 32);
            if (has_min) s += (half(p + 2) * da) * (float)a.s[b * ds + k];
            acc[b][k & 7] += s;
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8(acc[b]);
}

static void dot_k45(int t, const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    float acc[MAX_BATCH][8];
    int8_t w[32];
    memset(acc, 0, sizeof acc);
    for (int64_t k = 0; k < nb; k += 8, row += TYPES[t].size) {
        float d = half(row), dmin = half(row + 2);
        int sc[8], mn[8];
        kscales(row + 4, sc, mn);
        const unsigned char *qh = row + 16, *qsb = row + (t == T_Q5_K ? 48 : 16);
        for (int j = 0; j < 8; j++) {  /* sub-block j: the low (even j) or high nibbles of pair j / 2 */
            const unsigned char *q = qsb + 32 * (j / 2);
            if (j % 2) {
                for (int l = 0; l < 32; l++) w[l] = (int8_t)(q[l] >> 4);
            } else {
                for (int l = 0; l < 32; l++) w[l] = (int8_t)(q[l] & 15);
            }
            if (t == T_Q5_K)
                for (int l = 0; l < 32; l++) w[l] = (int8_t)(w[l] | ((qh[l] >> j) & 1) << 4);
            for (int b = 0; b < B; b++) {
                float da = a.d[b * ds + k + j];
                int32_t is = dot_i8(w, a.q + b * qs + 32 * (k + j), 32);
                acc[b][j] += (da * d) * (float)(sc[j] * is) - (da * dmin) * (float)(mn[j] * a.s[b * ds + k + j]);
            }
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8(acc[b]);
}

static void dot_q6k(const unsigned char *row, int64_t nb, Act a, int64_t qs, int64_t ds, int B, float *out) {
    float acc[MAX_BATCH][8];
    int8_t w[32];
    memset(acc, 0, sizeof acc);
    for (int64_t k = 0; k < nb; k += 8, row += 210) {
        float d = half(row + 208);
        for (int j = 0; j < 8; j++) {  /* block j: part t of half h */
            int h = j / 4, t = j % 4;
            const unsigned char *ql = row + 64 * h + 32 * (t & 1), *qh = row + 128 + 32 * h;
            const int8_t *sc = (const int8_t *)(row + 192) + 8 * h;
            if (t >= 2) {
                for (int l = 0; l < 32; l++) w[l] = (int8_t)(ql[l] >> 4);
            } else {
                for (int l = 0; l < 32; l++) w[l] = (int8_t)(ql[l] & 15);
            }
            for (int l = 0; l < 32; l++) w[l] = (int8_t)((w[l] | ((qh[l] >> (2 * t)) & 3) << 4) - 32);
            for (int b = 0; b < B; b++) {
                const int8_t *x = a.q + b * qs + 32 * (k + j);
                acc[b][j] += (a.d[b * ds + k + j] * d) * (float)(sc[2 * t] * dot_i8(w, x, 16) + sc[2 * t + 1] * dot_i8(w + 16, x + 16, 16));
            }
        }
    }
    for (int b = 0; b < B; b++) out[b] = sum8(acc[b]);
}
#endif

/* out[b] = a row of n weights of type t times vector b: x (floats, xs apart) for the float types, else a
 * (quantized: qs numbers and ds blocks apart). */
static void row_dots(int t, const unsigned char *w, int64_t n, const float *x, int64_t xs, Act a, int64_t qs,
                     int64_t ds, int B, float *out) {
    if (type_is_float(t)) dot_float(t, w, n, x, xs, B, out);
    else if (t == T_Q4_K || t == T_Q5_K) dot_k45(t, w, n / 32, a, qs, ds, B, out);
    else if (t == T_Q6_K) dot_q6k(w, n / 32, a, qs, ds, B, out);
    else dot_q32(t, w, n / 32, a, qs, ds, B, out);
}

/* a . b, floats (for the attention): sixteen running sums, as the products' */
static float vec_dot(const float *a, const float *b, int64_t n) {
#ifdef LLM_NEON
    float32x4_t acc[4] = {vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0)};
    int64_t i = 0;
    for (; i + 16 <= n; i += 16)
        for (int j = 0; j < 4; j++) acc[j] = vaddq_f32(acc[j], vmulq_f32(vld1q_f32(a + i + 4 * j), vld1q_f32(b + i + 4 * j)));
    if (i < n) {  /* (the end: padded with zeros) */
        float at[16] = {0}, bt[16] = {0};
        memcpy(at, a + i, sizeof(float) * (size_t)(n - i));
        memcpy(bt, b + i, sizeof(float) * (size_t)(n - i));
        for (int j = 0; j < 4; j++) acc[j] = vaddq_f32(acc[j], vmulq_f32(vld1q_f32(at + 4 * j), vld1q_f32(bt + 4 * j)));
    }
    return sum16_neon(acc);
#elif defined(LLM_SSE2)
    __m128 acc[4] = {_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()};
    int64_t i = 0;
    for (; i + 16 <= n; i += 16)
        for (int j = 0; j < 4; j++) acc[j] = _mm_add_ps(acc[j], _mm_mul_ps(_mm_loadu_ps(a + i + 4 * j), _mm_loadu_ps(b + i + 4 * j)));
    if (i < n) {
        float at[16] = {0}, bt[16] = {0};
        memcpy(at, a + i, sizeof(float) * (size_t)(n - i));
        memcpy(bt, b + i, sizeof(float) * (size_t)(n - i));
        for (int j = 0; j < 4; j++) acc[j] = _mm_add_ps(acc[j], _mm_mul_ps(_mm_loadu_ps(at + 4 * j), _mm_loadu_ps(bt + 4 * j)));
    }
    return sum16_sse(acc);
#else
    float acc[16] = {0};
    for (int64_t i = 0; i < n; i++) acc[i & 15] += a[i] * b[i];
    return sum16(acc);
#endif
}
