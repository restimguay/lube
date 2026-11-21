/*
 * vecmath.h - float32 functions written without libm and without
 * branches, so that a loop of them vectorizes: e^x, tanh, the sigmoid,
 * and GELU's normal distribution. They give the same bits on every system
 * (no fused multiply-adds: the plugins that include this turn them off),
 * and are accurate to a few units in the last place: exp within 1, the
 * sigmoid within 2.5, tanh within 1.5, GELU within 10 (even far into its
 * tail, where 0.5 x (1 + erf(x/sqrt 2)) loses everything). Results that
 * would be subnormal may come out as 0.
 * (For array and nn.)
 */
#ifndef LUBE_VECMATH_H
#define LUBE_VECMATH_H

#include <math.h>
#include <stdint.h>

static inline float vm_bits_f32(int32_t b) {
    union {
        int32_t i;
        float f;
    } u;
    u.i = b;
    return u.f;
}

static inline int32_t vm_f32_bits(float f) {
    union {
        int32_t i;
        float f;
    } u;
    u.f = f;
    return u.i;
}

/* e^(a + b), a exact and perhaps large, b small next to it: the range reduction subtracts from a first, so that
 * rounding a + b costs nothing (exp32 is b = 0). Below the smallest float it is 0, above the largest inf; NaN
 * stays NaN. 2^n is applied in two halves, so that n may reach the subnormals and 128. */
static inline float vm_exp_sum(float a, float b) {
    float x = a + b;
    float xc = x > -103.972084f ? (x < 88.7228394f ? x : 88.7228394f) : -103.972084f;  /* (NaN: the low end) */
    float t = xc * 1.44269504088896341f + 12582912.0f;  /* (1.5 2^23 rounds it to a whole number) */
    float n = t - 12582912.0f;
    float r = (a - n * 0.693359375f) + b;  /* (n ln 2 in two parts: the first exact) */
    r = r - n * -2.12194440e-4f;
    float p = 1.9875691500E-4f;  /* (Cephes' expf) */
    p = p * r + 1.3981999507E-3f;
    p = p * r + 8.3334519073E-3f;
    p = p * r + 4.1665795894E-2f;
    p = p * r + 1.6666665459E-1f;
    p = p * r + 5.0000001201E-1f;
    p = p * (r * r) + r + 1.0f;
    int32_t ni = (int32_t)n, h = ni / 2;
    float e = p * vm_bits_f32((h + 127) << 23) * vm_bits_f32((ni - h + 127) << 23);
    e = x > 88.7228394f ? INFINITY : e;
    e = x < -103.972084f ? 0.0f : e;
    return x != x ? x : e;
}

static inline float vm_exp(float x) { return vm_exp_sum(x, 0.0f); }

static inline float vm_sigmoid(float x) { return 1.0f / (1.0f + vm_exp(-x)); }

static inline float vm_tanh(float x) {
    float a = x < 0 ? -x : x, s = x * x;
    float small = ((((-5.70498872745E-3f * s + 2.06390887954E-2f) * s - 5.37397155531E-2f) * s + 1.33314422036E-1f) * s -
                   3.33332819422E-1f) * s * x + x;  /* (Cephes' tanhf, below 0.625) */
    float big = 1.0f - 2.0f / (vm_exp(a + a) + 1.0f);
    big = x < 0 ? -big : big;
    return a < 0.625f ? small : big;
}

/* GELU's normal distribution, in two parts, so that each loop holds few constants: phi_part gives t and the
 * exponent's two parts for |x|, phi_tail the tail probability P(Z > |x|) = erfc(|x|/sqrt 2)/2 from them
 * (Numerical Recipes' erfcc, within about 1e-7 of it, relatively; x^2/2 worked out exactly, from x split in
 * halves whose products are exact). */
static inline void vm_phi_part(float x, float *t_out, float *ea, float *eb) {
    float ax = x < 0 ? -x : x;
    ax = ax < 20.0f ? ax : 20.0f;  /* (the tail is 0 long before: so inf and NaN stay out of it) */
    float z = ax * 0.70710678118654752f;
    float xh = vm_bits_f32(vm_f32_bits(ax) & (int32_t)0xFFFFF000), xl = ax - xh;
    float t = 1.0f / (1.0f + 0.5f * z);
    float p = 0.17087277f;
    p = p * t - 0.82215223f;
    p = p * t + 1.48851587f;
    p = p * t - 1.13520398f;
    p = p * t + 0.27886807f;
    p = p * t - 0.18628806f;
    p = p * t + 0.09678418f;
    p = p * t + 0.37409196f;
    p = p * t + 1.00002368f;
    p = p * t - 1.26551223f;
    *t_out = t;
    *ea = -(xh * xh) * 0.5f;
    *eb = p - (xh * xl + 0.5f * (xl * xl));
}

static inline float vm_phi_tail(float t, float ea, float eb) { return 0.5f * t * vm_exp_sum(ea, eb); }

/* GELU, x P(Z < x), from h = P(Z > |x|) */
static inline float vm_gelu_of(float x, float h) {
    float g = x * (x < 0 ? h : 1.0f - h);
    return x < -14.0f ? -0.0f : g;  /* (below: 0 to a float; and -inf times 0 is not) */
}

/* e^(-x^2/2), x^2 worked out exactly as above (for GELU's slope) */
static inline float vm_gauss(float x) {
    float ax = x < 0 ? -x : x;
    ax = ax < 20.0f ? ax : 20.0f;
    float xh = vm_bits_f32(vm_f32_bits(ax) & (int32_t)0xFFFFF000), xl = ax - xh;
    return vm_exp_sum(-(xh * xh) * 0.5f, -(xh * xl + 0.5f * (xl * xl)));
}

#endif
