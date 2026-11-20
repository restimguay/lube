/*
 * array/pool.c - threads that share out big loops: par_for(n, grain, fn)
 * runs fn over pieces of 0..n on the caller's thread and on workers that
 * wait for the next job. One job at a time: an isolate that finds the
 * workers busy runs its loop on its own thread. (Part of array.c.)
 */

#define MAX_THREADS 64

static LubeLock *pool_lock;
static int pool_workers;       /* workers started so far */
static int pool_busy;          /* a job is on (under pool_lock, as everything here) */
static int thread_limit;       /* how many threads loops may use (threads(n)); 0: one per core */
static int core_count;

static struct {
    void (*fn)(void *ctx, int64_t from, int64_t to);
    void *ctx;
    int64_t n, piece, next;    /* 0..n in pieces of `piece`: the next to take */
    unsigned waiting;          /* workers yet to see this job through */
    int active;                /* how many of them take part (threads(n) may want fewer than there are) */
    unsigned serial;           /* which job: each worker takes part in each one once */
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

/* How many threads a loop uses now. */
static int threads_now(void) {
    int n = thread_limit ? thread_limit : core_count;
    return n < 1 ? 1 : n > MAX_THREADS ? MAX_THREADS : n;
}

/* job.serial and job.waiting are also read without the lock, while spinning: atomically, always. */
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define SPIN 20000  /* checks before sleeping: tens of microseconds, about what waking a thread costs */

/* Takes pieces of the job until none are left (under the lock, which it lets go of while working). */
static void take_pieces(void) {
    while (job.next < job.n) {
        int64_t from = job.next, to = from + job.piece < job.n ? from + job.piece : job.n;
        job.next = to;
        lube->lock_release(pool_lock);
        job.fn(job.ctx, from, to);
        lube->lock_acquire(pool_lock);
    }
}

/* Waits (without the lock) until *x differs from was: spinning a little first, as a loop's next job often
 * comes at once, then asleep. */
static void wait_change(unsigned *x, unsigned was) {
    for (int i = 0; i < SPIN; i++)
        if (LOAD(*x) != was) return;
    lube->lock_acquire(pool_lock);
    while (LOAD(*x) == was) lube->lock_wait(pool_lock);
    lube->lock_release(pool_lock);
}

static void worker(void *arg) {
    int id = (int)(intptr_t)arg;
    unsigned seen = 0;
    for (;;) {
        wait_change(&job.serial, seen);
        lube->lock_acquire(pool_lock);
        seen = LOAD(job.serial);
        if (id < job.active) take_pieces();
        STORE(job.waiting, LOAD(job.waiting) - 1);
        lube->lock_notify(pool_lock);  /* (the caller may be asleep) */
        lube->lock_release(pool_lock);
    }
}

static void par_for(int64_t n, int64_t grain, void (*fn)(void *ctx, int64_t from, int64_t to), void *ctx) {
    int t = threads_now();
    if (grain < 1) grain = 1;
    if (t <= 1 || n <= grain) {
        if (n > 0) fn(ctx, 0, n);
        return;
    }
    lube->lock_acquire(pool_lock);
    if (pool_busy) {  /* LCOV_EXCL_START: another isolate's loop at that very moment */
        lube->lock_release(pool_lock);
        fn(ctx, 0, n);
        return;
    }  /* LCOV_EXCL_STOP */
    while (pool_workers < t - 1 && lube->thread_start(worker, (void *)(intptr_t)pool_workers, 256 * 1024)) pool_workers++;
    pool_busy = 1;
    int64_t pieces = n / grain < (int64_t)t * 4 ? n / grain : (int64_t)t * 4;  /* (a few each: some finish early) */
    job.fn = fn;
    job.ctx = ctx;
    job.n = n;
    job.piece = (n + pieces - 1) / pieces;
    job.next = 0;
    job.active = t - 1;
    STORE(job.waiting, (unsigned)pool_workers);
    STORE(job.serial, LOAD(job.serial) + 1);
    lube->lock_notify(pool_lock);
    take_pieces();
    lube->lock_release(pool_lock);
    for (int i = 0; i < SPIN && LOAD(job.waiting); i++) {}  /* (the others finish about when this thread does) */
    lube->lock_acquire(pool_lock);
    while (LOAD(job.waiting)) lube->lock_wait(pool_lock);
    pool_busy = 0;
    lube->lock_release(pool_lock);
}
