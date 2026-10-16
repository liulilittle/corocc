#ifndef COROCC_TESTS_PALT_THREADS_H
#define COROCC_TESTS_PALT_THREADS_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/palt_threads.h -- a thin thread-compatibility layer for the
 * concurrency suites.
 *
 * The concurrency suites were written against pthreads, which does not exist
 * on Windows. This layer gives them one vocabulary to write against:
 *
 *   POSIX   -- the pt_* wrappers forward straight to pthreads. macOS has no
 *              POSIX barrier (there is no pthread_barrier_t in the SDK
 *              headers), so there the barrier is hand-rolled from
 *              mutex+condition with the same phase semantics.
 *   Windows -- SRWLOCK for mutexes, CONDITION_VARIABLE for conditions, a
 *              hand-rolled mutex+condition barrier, and _beginthreadex for
 *              threads (the CRT-safe entry, joined through
 *              WaitForSingleObject + CloseHandle).
 *
 * It is deliberately tiny and not a general pthread emulation: only the
 * operations the suites use are provided, and the semantics they rely on
 * (mutual exclusion, wait/broadcast, a whole-phase barrier, join) are the
 * portable core shared by both backends. The header is INTERNAL to the
 * tests; it is not installed.
 */

#if defined(_WIN32)

#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef HANDLE             pt_thread_t;
typedef SRWLOCK            pt_mutex_t;  /* zero-initialization is legal */
typedef CONDITION_VARIABLE pt_cond_t;   /* zero-initialization is legal */

#define PT_MUTEX_INITIALIZER SRWLOCK_INIT
#define PT_COND_INITIALIZER  {0}

typedef struct pt_barrier {
    SRWLOCK            lock;  /* zero-init before pt_barrier_init runs */
    CONDITION_VARIABLE cv;
    unsigned           count; /* parties per phase */
    unsigned           left;  /* parties still to arrive */
    unsigned           gen;   /* phase counter: broadcast gates on it */
} pt_barrier_t;

static inline int pt_mutex_init(pt_mutex_t* m) {
    InitializeSRWLock(m);
    return 0;
}
static inline int pt_mutex_lock(pt_mutex_t* m) {
    AcquireSRWLockExclusive(m);
    return 0;
}
static inline int pt_mutex_unlock(pt_mutex_t* m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}
static inline int pt_cond_init(pt_cond_t* c) {
    InitializeConditionVariable(c);
    return 0;
}
static inline int pt_cond_wait(pt_cond_t* c, pt_mutex_t* m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}
static inline int pt_cond_broadcast(pt_cond_t* c) {
    WakeAllConditionVariable(c);
    return 0;
}
static inline int pt_cond_signal(pt_cond_t* c) {
    WakeConditionVariable(c);
    return 0;
}

static inline int pt_barrier_init(pt_barrier_t* b, unsigned count) {
    InitializeSRWLock(&b->lock);
    InitializeConditionVariable(&b->cv);
    b->count = count;
    b->left = 0;
    b->gen = 0;
    return 0;
}

/* Releases every waiter of the current phase together; exactly one caller
   sees 1 (the POSIX backend's PTHREAD_BARRIER_SERIAL_THREAD analogue), the
   rest see 0. */
static inline int pt_barrier_wait(pt_barrier_t* b) {
    int last = 0;
    unsigned gen0;
    AcquireSRWLockExclusive(&b->lock);
    gen0 = b->gen;
    if (++b->left == b->count) {
        b->left = 0;
        ++b->gen;
        last = 1;
        WakeAllConditionVariable(&b->cv);
    } else {
        while (b->gen == gen0) {
            SleepConditionVariableSRW(&b->cv, &b->lock, INFINITE, 0);
        }
    }
    ReleaseSRWLockExclusive(&b->lock);
    return last;
}

typedef struct pt_thread_start {
    void* (*fn)(void*);
    void*   arg;
} pt_thread_start;

static unsigned __stdcall pt_thread_entry(void* p) {
    pt_thread_start* s = (pt_thread_start*)p;
    void* (*fn)(void*) = s->fn;
    void* arg = s->arg;
    free(s);
    fn(arg);
    return 0;
}

static inline int pt_thread_create(pt_thread_t* t, void* (*fn)(void*),
                                   void* arg) {
    pt_thread_start* s;
    uintptr_t h;
    s = (pt_thread_start*)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    h = _beginthreadex(NULL, 0, pt_thread_entry, s, 0, NULL);
    if (h == 0) {
        free(s);
        return -1;
    }
    *t = (HANDLE)h;
    return 0;
}

static inline int pt_thread_join(pt_thread_t t) {
    DWORD rc = WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}

#else /* !_WIN32: forwards to pthreads (barrier by hand on macOS) */

#include <pthread.h>

typedef pthread_t       pt_thread_t;
typedef pthread_mutex_t pt_mutex_t;
typedef pthread_cond_t  pt_cond_t;

#define PT_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define PT_COND_INITIALIZER  PTHREAD_COND_INITIALIZER

static inline int pt_mutex_init(pt_mutex_t* m) {
    return pthread_mutex_init(m, NULL);
}
static inline int pt_mutex_lock(pt_mutex_t* m) {
    return pthread_mutex_lock(m);
}
static inline int pt_mutex_unlock(pt_mutex_t* m) {
    return pthread_mutex_unlock(m);
}
static inline int pt_cond_init(pt_cond_t* c) {
    return pthread_cond_init(c, NULL);
}
static inline int pt_cond_wait(pt_cond_t* c, pt_mutex_t* m) {
    return pthread_cond_wait(c, m);
}
static inline int pt_cond_broadcast(pt_cond_t* c) {
    return pthread_cond_broadcast(c);
}
static inline int pt_cond_signal(pt_cond_t* c) {
    return pthread_cond_signal(c);
}

#if defined(__APPLE__)
/* macOS has no POSIX barrier: current SDK headers declare no
   pthread_barrier_t / pthread_barrier_init (verified against the macOS 15.5
   SDK before this branch was added). The hand-rolled phase barrier below
   gives the suites the same contract the pthread_barrier_* family does:
   every party of a phase is released together, and exactly one waiter sees
   1 (the PTHREAD_BARRIER_SERIAL_THREAD analogue). Same algorithm as the
   Windows branch above. */
typedef struct pt_barrier {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    unsigned        count; /* parties per phase */
    unsigned        left;  /* parties still to arrive */
    unsigned        gen;   /* phase counter: broadcast gates on it */
} pt_barrier_t;

static inline int pt_barrier_init(pt_barrier_t* b, unsigned count) {
    if (pthread_mutex_init(&b->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&b->cv, NULL) != 0) {
        pthread_mutex_destroy(&b->lock);
        return -1;
    }
    b->count = count;
    b->left = 0;
    b->gen = 0;
    return 0;
}

static inline int pt_barrier_wait(pt_barrier_t* b) {
    int last = 0;
    unsigned gen0;
    pthread_mutex_lock(&b->lock);
    gen0 = b->gen;
    if (++b->left == b->count) {
        b->left = 0;
        ++b->gen;
        last = 1;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (b->gen == gen0) {
            pthread_cond_wait(&b->cv, &b->lock);
        }
    }
    pthread_mutex_unlock(&b->lock);
    return last;
}
#else
typedef pthread_barrier_t pt_barrier_t;

static inline int pt_barrier_init(pt_barrier_t* b, unsigned count) {
    return pthread_barrier_init(b, NULL, count);
}
static inline int pt_barrier_wait(pt_barrier_t* b) {
    return pthread_barrier_wait(b);
}
#endif /* __APPLE__ */

static inline int pt_thread_create(pt_thread_t* t, void* (*fn)(void*),
                                   void* arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline int pt_thread_join(pt_thread_t t) {
    void* result = NULL;
    return pthread_join(t, &result);
}

#endif /* !_WIN32 */

#endif /* COROCC_TESTS_PALT_THREADS_H */
