/*
 * thread.c - the little of the operating system that plain C lacks: threads
 * and a lock with a condition to wait on (for isolates), and listing a
 * folder (for packages); on Windows, also the path of the running program
 * (for lube --build). POSIX on macOS, Linux, iOS and Android; Win32 on
 * Windows. Both are part of the system's own C library.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* pthreads and sysconf(_SC_NPROCESSORS_ONLN) under -std=c99 (macOS shows them anyway) */
#endif
#include "lube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Adds `name` to a list of file names if it ends in .lube. */
static void add_lube(char ***names, int *n, int *cap, const char *name) {
    size_t len = strlen(name);
    if (len <= 5 || strcmp(name + len - 5, ".lube")) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *names = xrealloc(*names, sizeof(char *) * (size_t)*cap);
    }
    (*names)[(*n)++] = xstrndup(name, len);
}

#ifdef _WIN32
#define TokenType WinTokenType  /* winnt.h has a TokenType of its own */
#include <windows.h>
#undef TokenType

struct LubeLock {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
};

LubeLock *lock_new(void) {
    LubeLock *l = xmalloc(sizeof(LubeLock));
    InitializeCriticalSection(&l->cs);
    InitializeConditionVariable(&l->cv);
    return l;
}

void lock_acquire(LubeLock *l) { EnterCriticalSection(&l->cs); }
void lock_release(LubeLock *l) { LeaveCriticalSection(&l->cs); }
void lock_wait(LubeLock *l) { SleepConditionVariableCS(&l->cv, &l->cs, INFINITE); }
void lock_notify(LubeLock *l) { WakeAllConditionVariable(&l->cv); }

typedef struct {
    void (*fn)(void *);
    void *arg;
} Start;

static DWORD WINAPI thread_main(LPVOID p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int thread_start(void (*fn)(void *), void *arg, size_t stack_size) {
    Start *s = xmalloc(sizeof(Start));
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, stack_size, thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        free(s);
        return 0;
    }
    CloseHandle(h);  /* detached: isolates report back through their own lock */
    return 1;
}

/* The .lube files in a folder, sorted (NULL with *n = -1 if it is not one). */
char **os_lube_files(const char *dir, int *n) {
    char pattern[4096];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    *n = -1;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char **names = NULL;
    int cap = 0;
    *n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add_lube(&names, n, &cap, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (*n) qsort(names, (size_t)*n, sizeof(char *), by_name);
    return names;
}

/* The running program's path (malloc'd), or NULL. */
char *os_self_path(void) {
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? xstrndup(buf, n) : NULL;
}

int lube_cores(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

#else
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

/* The .lube files in a folder, sorted (NULL with *n = -1 if it is not one). */
char **os_lube_files(const char *dir, int *n) {
    DIR *d = opendir(dir);
    *n = -1;
    if (!d) return NULL;
    char **names = NULL;
    int cap = 0;
    struct dirent *e;
    *n = 0;
    while ((e = readdir(d))) add_lube(&names, n, &cap, e->d_name);
    closedir(d);
    if (*n) qsort(names, (size_t)*n, sizeof(char *), by_name);
    return names;
}

struct LubeLock {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

LubeLock *lock_new(void) {
    LubeLock *l = xmalloc(sizeof(LubeLock));
    pthread_mutex_init(&l->mutex, NULL);
    pthread_cond_init(&l->cond, NULL);
    return l;
}

void lock_acquire(LubeLock *l) { pthread_mutex_lock(&l->mutex); }
void lock_release(LubeLock *l) { pthread_mutex_unlock(&l->mutex); }
void lock_wait(LubeLock *l) { pthread_cond_wait(&l->cond, &l->mutex); }
void lock_notify(LubeLock *l) { pthread_cond_broadcast(&l->cond); }

typedef struct {
    void (*fn)(void *);
    void *arg;
} Start;

static void *thread_main(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

/* Starts fn(arg) on a new thread with its own stack. Returns 0 if the
 * system would not start one. */
int thread_start(void (*fn)(void *), void *arg, size_t stack_size) {
    pthread_attr_t attr;
    pthread_t t;
    Start *s = xmalloc(sizeof(Start));
    s->fn = fn;
    s->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);  /* isolates report back through their own lock */
    int ok = pthread_create(&t, &attr, thread_main, s) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) free(s); /* LCOV_EXCL_LINE: only when the system has no threads left */
    return ok;
}

int lube_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif
