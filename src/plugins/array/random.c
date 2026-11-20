/*
 * array/random.c - random Arrays, giving the numbers numpy's np.random gives
 * (its RandomState: the Mersenne Twister, seeded and drawn from as numpy
 * does) for the same seed. One generator for the process, under a lock.
 * (Part of array.c.)
 */

typedef struct {
    uint32_t key[624];
    int pos;
    int has_gauss;
    double gauss;
} Twister;

static Twister rng;
static int rng_seeded;
static LubeLock *rng_lock;

static void rng_seed(uint32_t seed) {
    for (int i = 0; i < 624; i++) {
        rng.key[i] = seed;
        seed = 1812433253u * (seed ^ (seed >> 30)) + (uint32_t)i + 1;
    }
    rng.pos = 624;
    rng.has_gauss = 0;
    rng.gauss = 0;
    rng_seeded = 1;
}

static uint32_t next32(void) {
    if (rng.pos == 624) {
        for (int i = 0; i < 624; i++) {
            uint32_t y = (rng.key[i] & 0x80000000u) | (rng.key[(i + 1) % 624] & 0x7fffffffu);
            rng.key[i] = rng.key[(i + 397) % 624] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfu : 0);
        }
        rng.pos = 0;
    }
    uint32_t y = rng.key[rng.pos++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

static uint64_t next64(void) {
    uint64_t hi = next32();
    return hi << 32 | next32();
}

static double next_double(void) {  /* [0, 1) with 53 random bits */
    uint32_t a = next32() >> 5, b = next32() >> 6;
    return (a * 67108864.0 + b) / 9007199254740992.0;
}

static double next_gauss(void) {  /* numpy's legacy polar method, keeping the second value for next time */
    if (rng.has_gauss) {
        rng.has_gauss = 0;
        return rng.gauss;
    }
    double x1, x2, r2;
    do {
        x1 = 2.0 * next_double() - 1.0;
        x2 = 2.0 * next_double() - 1.0;
        r2 = x1 * x1 + x2 * x2;
    } while (r2 >= 1.0 || r2 == 0.0);
    double f = sqrt(-2.0 * log(r2) / r2);
    rng.gauss = f * x1;
    rng.has_gauss = 1;
    return f * x2;
}

/* A whole number in 0..max, every one as likely (numpy's masked rejection). */
static uint64_t next_below(uint64_t max) {
    if (max == 0) return 0;
    uint64_t mask = max, v;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    if (max <= 0xffffffffu) {
        while ((v = (next32() & mask)) > max) {}
    } else {
        while ((v = (next64() & mask)) > max) {}
    }
    return v;
}

/* Under the lock, seeded from the clock if no seed() came first. */
static void rng_lock_on(void) {
    lube->lock_acquire(rng_lock);
    if (!rng_seeded) rng_seed((uint32_t)time(NULL) ^ (uint32_t)(uint64_t)(lube->now() * 1e6));
}

static void rng_lock_off(void) { lube->lock_release(rng_lock); }
