/*
 * array/pool.c - threads that share out big loops: par_for(n, grain, fn)
 * runs fn over pieces of 0..n on the caller's thread and on workers that
 * wait for the next job. One job at a time: an isolate that finds the
 * workers busy runs its loop on its own thread. (Part of array.c.)
 */

#define MAX_THREADS 64

static LubeLock *pool_lock;     /* for sleeping and waking only: pieces are taken without it */
static int pool_workers;        /* workers started so far (under pool_lock) */
static int pool_busy;           /* a job is on: another isolate's loop runs on its own thread meanwhile */
static int thread_limit;        /* how many threads loops may use (threads(n)); 0: one per core */
static int core_count;
static unsigned pool_sleepers;  /* workers asleep, or about to be */

/* The job: fn over 0..n in `pieces` pieces of `piece`. Who takes a piece takes it from `claim`, one word: the
 * job's number (16 bits), its pieces (24) and the next piece to take (24), so that a worker that wakes up late
 * cannot take a piece of another job by mistake. The fields are written before claim is, and stay until every
 * piece is done. */
static struct {
    void (*fn)(void *ctx, int64_t from, int64_t to);
    void *ctx;
    int64_t n, piece;
    uint64_t claim;
    unsigned done;              /* pieces finished */
    unsigned serial;            /* which job: it changes to wake the workers */
    int active;                 /* how many workers take part (threads(n) may want fewer than there are) */
} job;

static int cores(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;  /* LCOV_EXCL_LINE: (some count of cores, always) */
#endif
}

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define LOAD_SC(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define STORE_SC(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define SPIN 50000  /* checks before sleeping, a pause each: some tens of microseconds, so that the next of a run of
                     * operations finds the workers awake (waking a sleeping one costs about as much) */
#if defined(__aarch64__) || defined(__arm64__)
#define RELAX() __asm__ __volatile__("yield")
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define RELAX() __asm__ __volatile__("pause")
#else
#define RELAX() ((void)0)
#endif
#define CLAIM(serial, pieces, next) ((uint64_t)((serial) & 0xFFFF) << 48 | (uint64_t)(pieces) << 24 | (uint64_t)(next))

/* Takes pieces of job `serial` and runs them, until none are left; wakes the caller after the last. */
static void take_pieces(unsigned serial) {
    for (;;) {
        uint64_t c = LOAD(job.claim);
        unsigned pieces = (unsigned)(c >> 24 & 0xFFFFFF), next = (unsigned)(c & 0xFFFFFF);
        if ((c >> 48) != (serial & 0xFFFF) || next >= pieces) return;
        if (!__atomic_compare_exchange_n(&job.claim, &c, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        int64_t from = (int64_t)next * job.piece, to = from + job.piece < job.n ? from + job.piece : job.n;
        job.fn(job.ctx, from, to);
        if (__atomic_add_fetch(&job.done, 1, __ATOMIC_ACQ_REL) == pieces) {  /* (the caller may be asleep) */
            lube->lock_acquire(pool_lock);
            lube->lock_notify(pool_lock);
            lube->lock_release(pool_lock);
        }
    }
}

/* Waits until job.serial differs from was: spinning a while first, as a loop's next job often comes soon, then
 * asleep. */
static unsigned wait_job(unsigned was) {
    for (int i = 0; i < SPIN; i++) {
        unsigned s = LOAD(job.serial);
        if (s != was) return s;
        RELAX();
    }
    lube->lock_acquire(pool_lock);
    STORE_SC(pool_sleepers, LOAD(pool_sleepers) + 1);  /* (counted before looking again: see par_for) */
    while (LOAD_SC(job.serial) == was) lube->lock_wait(pool_lock);
    STORE(pool_sleepers, LOAD(pool_sleepers) - 1);
    lube->lock_release(pool_lock);
    return LOAD(job.serial);
}

static void worker(void *arg) {
    int id = (int)(intptr_t)arg;
    unsigned seen = 0;
    for (;;) {
        seen = wait_job(seen);
        if (id < LOAD(job.active)) take_pieces(seen);
    }
}

/* How many threads a loop uses now. */
static int threads_now(void) {
    int limit = LOAD(thread_limit), n = limit ? limit : core_count;
    return n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
}

static void par_for(int64_t n, int64_t grain, void (*fn)(void *ctx, int64_t from, int64_t to), void *ctx) {
    int t = threads_now();
    if (grain < 1) grain = 1;
    int busy = 0;
    if (t <= 1 || n / grain < 2 || !__atomic_compare_exchange_n(&pool_busy, &busy, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (n > 0) fn(ctx, 0, n);  /* (one piece, or another isolate's loop on at that very moment: here) */
        return;
    }
    lube->lock_acquire(pool_lock);
    while (pool_workers < t - 1 && lube->thread_start(worker, (void *)(intptr_t)pool_workers, 256 * 1024)) pool_workers++;
    lube->lock_release(pool_lock);
    int64_t pieces = n / grain < (int64_t)t * 4 ? n / grain : (int64_t)t * 4;  /* (a few each: some finish early) */
    unsigned serial = LOAD(job.serial) + 1;
    job.fn = fn;
    job.ctx = ctx;
    job.n = n;
    job.piece = (n + pieces - 1) / pieces;
    pieces = (n + job.piece - 1) / job.piece;
    STORE(job.active, t - 1);
    STORE(job.done, 0u);
    STORE(job.claim, CLAIM(serial, pieces, 0));
    STORE_SC(job.serial, serial);
    if (LOAD_SC(pool_sleepers)) {  /* (a worker counts itself before it looks at serial: none is missed) */
        lube->lock_acquire(pool_lock);
        lube->lock_notify(pool_lock);
        lube->lock_release(pool_lock);
    }
    take_pieces(serial);
    for (int i = 0; i < SPIN && LOAD(job.done) != (unsigned)pieces; i++) RELAX();  /* (the others end about now) */
    if (LOAD(job.done) != (unsigned)pieces) {
        lube->lock_acquire(pool_lock);
        while (LOAD(job.done) != (unsigned)pieces) lube->lock_wait(pool_lock);
        lube->lock_release(pool_lock);
    }
    STORE(pool_busy, 0);
}
