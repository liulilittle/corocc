#ifndef COROCC_ARCH_ATOMIC_H
#define COROCC_ARCH_ATOMIC_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/arch/coro_atomic.h -- internal 64-bit atomic helpers.
 *
 * The library is built as C99 and must also compile under MSVC, so the C11
 * <stdatomic.h> header is deliberately NOT used. Instead the compiler's own
 * atomics are wrapped: GCC/Clang __atomic builtins and the MSVC interlocked
 * intrinsics. The result is a tiny, dependency-free set of acquire/release
 * operations on a 64-bit word, which is the width the state word and the
 * monotone token sequence need.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI. The MSVC path cannot be exercised until the milestone-M9
 * Windows backend lands; it is written against the documented intrinsic
 * contract and is verified by CI as soon as that job exists.
 */
#include <stdint.h>

/* Zero-spin verification hook. With COROCC_PROBE_NOSPIN defined, every
   compare-and-swap this header performs is counted in a global, so a test can
   prove that a control path performs exactly one CAS (no retry loop). The
   counter is defined by the library (coro.c) and read by tests. */
#ifdef COROCC_PROBE_NOSPIN
extern unsigned long coro_probe_cas_count;
#endif

#if defined(__GNUC__) || defined(__clang__)

/* GCC and Clang: __atomic builtins map onto the target's lock-free atomics
   where available, and libatomic otherwise. */

static inline uint64_t coro_at_load(const volatile uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void coro_at_store(volatile uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* Strong compare-exchange: exactly one attempt, never a retry loop. Returns
   1 and updates *expected to the observed value on failure, so the caller can
   re-read the state without spinning. */
static inline int coro_at_cas(volatile uint64_t* p, uint64_t* expected,
                              uint64_t desired) {
#ifdef COROCC_PROBE_NOSPIN
    ++coro_probe_cas_count;
#endif
    return __atomic_compare_exchange_n(p, expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Monotone counter source. Uniqueness does not require ordering, so the add is
   relaxed; publication of the resulting value uses coro_at_store (release). */
static inline uint64_t coro_at_fetch_add(volatile uint64_t* p, uint64_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

/* Read-and-swap: returns the previous value and stores `v`. The swap is fully
   ordered (ACQ_REL), which is what the MPSC queue needs to drain a batch. */
static inline uint64_t coro_at_exchange(volatile uint64_t* p, uint64_t v) {
    return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}

/* Bitwise-OR, relaxed: used for status flags where a stale reader only needs
   to notice the bit latched. */
static inline uint64_t coro_at_or(volatile uint64_t* p, uint64_t v) {
    return __atomic_fetch_or(p, v, __ATOMIC_RELAXED);
}

/* Bitwise-AND, relaxed: the mirror of coro_at_or for clearing flags. */
static inline uint64_t coro_at_and(volatile uint64_t* p, uint64_t v) {
    return __atomic_fetch_and(p, v, __ATOMIC_RELAXED);
}

/* Full memory barrier. The Chase-Lev deque needs it twice: the owner orders
   its bottom store before the top load (pop), the thief orders its top load
   before the bottom load (steal). Without these, on TSO the single-element
   race can let both sides take the last node. */
static inline void coro_at_fence(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Retry loop for lock-free queues: returns the value the location held after
   the winning attempt. Bounded by the caller; only the queue layer may loop. */
static inline uint64_t coro_at_cas_value(volatile uint64_t* p,
                                        uint64_t expected, uint64_t desired) {
    for (;;) {
#ifdef COROCC_PROBE_NOSPIN
        ++coro_probe_cas_count;
#endif
        if (__atomic_compare_exchange_n(p, &expected, desired, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return expected;
        }
    }
}

#elif defined(_MSC_VER)

#include <intrin.h>

/* MSVC has no plain 64-bit load intrinsic; a compare-exchange against the
   current value both reads it and leaves it untouched. */
static inline uint64_t coro_at_load(const volatile uint64_t* p) {
    return (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
}

static inline void coro_at_store(volatile uint64_t* p, uint64_t v) {
    _InterlockedExchange64((volatile long long*)p, (long long)v);
}

static inline int coro_at_cas(volatile uint64_t* p, uint64_t* expected,
                              uint64_t desired) {
    uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, (long long)desired, (long long)*expected);
    if (observed == *expected) return 1;
    *expected = observed;
    return 0;
}

static inline uint64_t coro_at_fetch_add(volatile uint64_t* p, uint64_t v) {
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long*)p,
                                               (long long)v);
}

static inline uint64_t coro_at_exchange(volatile uint64_t* p, uint64_t v) {
    return (uint64_t)_InterlockedExchange64((volatile long long*)p,
                                            (long long)v);
}

static inline uint64_t coro_at_or(volatile uint64_t* p, uint64_t v) {
    return (uint64_t)_InterlockedOr64((volatile long long*)p, (long long)v);
}

static inline uint64_t coro_at_and(volatile uint64_t* p, uint64_t v) {
    return (uint64_t)_InterlockedAnd64((volatile long long*)p, (long long)v);
}

static inline void coro_at_fence(void) {
    _ReadWriteBarrier();
    MemoryBarrier();
}

static inline uint64_t coro_at_cas_value(volatile uint64_t* p,
                                        uint64_t expected, uint64_t desired) {
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)desired, (long long)expected);
        if (observed == expected) return expected;
        expected = observed;
    }
}

#else
#error "corocc: no 64-bit atomic implementation for this compiler (GCC, Clang or MSVC required)"
#endif

#endif /* COROCC_ARCH_ATOMIC_H */
