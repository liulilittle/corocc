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
 * API or ABI. The MSVC path is written against the documented intrinsic
 * contract and is verified by CI on the Windows job.
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

/* Release-OR: the publication form for the retire's RT_DONE write.
   A relaxed fetch_or latches the bit; the RELEASE ordering additionally
   publishes every write this thread made before it (the wait-chain detach
   and the retained reading) to the thread whose ACQUIRE load observes the
   bit. The MSVC interlocked OR is already a full barrier, so it needs no
   separate form. */
static inline uint64_t coro_at_or_release(volatile uint64_t* p, uint64_t v) {
    return __atomic_fetch_or(p, v, __ATOMIC_RELEASE);
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

/* Barrier RMW: a zero-delta fetch-add on a 64-bit word the caller's
   protocol already owns (the lock word or the semaphore count). It is an
   acquire/release RMW on that single location -- on x86 the locked
   instruction is also a full barrier, but on the ARM family (ldaddal, or
   the ldrex/strex pair; no separate dmb), on riscv64 (amoadd.d.aqrl), on
   s390x (laag) and on the other supported targets it is exactly the
   acquire/release semantics of that RMW, NOT a global full barrier. The
   sync fast paths rely only on the same-location release/acquire chain
   such an RMW pair establishes (the enqueue or the release is published
   before this RMW; a later RMW on the same word reads that write and
   carries the edge onward), never on a full barrier. Chosen over
   __atomic_thread_fence because GCC's TSan instrumentation rejects the
   fence builtin under -Wtsan, and a modeled RMW keeps the TSan site
   warning-free. */
static inline void coro_at_fence_rmw(volatile uint64_t* p) {
    (void)__atomic_fetch_add(p, (uint64_t)0, __ATOMIC_SEQ_CST);
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

/* Pointer-width (uintptr_t) atomics. Fields that hold pointers or size_t must
   be accessed with exactly the machine-word width: on ILP32 a uint64_t access
   would read or write 8 bytes over a 4-byte object. On LP64 these compile to
   the same 8-byte atomic operations as the coro_at_* family above. */
static inline uintptr_t coro_atp_load(const volatile uintptr_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void coro_atp_store(volatile uintptr_t* p, uintptr_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline int coro_atp_cas(volatile uintptr_t* p, uintptr_t* expected,
                               uintptr_t desired) {
#ifdef COROCC_PROBE_NOSPIN
    ++coro_probe_cas_count;
#endif
    return __atomic_compare_exchange_n(p, expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline uintptr_t coro_atp_exchange(volatile uintptr_t* p, uintptr_t v) {
    return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}

#elif defined(_MSC_VER)

#include <intrin.h>
#if defined(_M_IX86)
#include <emmintrin.h> /* _mm_mfence: the x86 full-fence spelling below */
#endif

/* The intrinsics below take the address of a 64-bit word, and the wrappers
   cast freely between uint64_t*, long long* and void*. Those casts are only
   sound while the atomic word really is one 8-byte, 8-byte-aligned unit
   (aligned 8-byte accesses are also the ones the hardware keeps atomic).
   C99 has no static_assert, so the negative-array-size trick carries the
   check; MSVC's C front end supports __alignof. */
typedef char coro_atomic_word_size_must_be_8[(sizeof(uint64_t) == 8) ? 1 : -1];
typedef char coro_atomic_word_align_must_be_8[(__alignof(uint64_t) == 8) ? 1 : -1];

#if defined(_M_IX86)
/* x86: of the 64-bit interlocked family only the compare-exchange form (and
   the pointer-width forms) is declared for 32-bit targets -- the
   exchange/add/or/and intrinsics are x64/ARM declarations, rejected as
   undeclared by the VS2022 x86 toolset (C4013; the Windows x86 job is the
   evidence). Where they exist they compile to a locked cmpxchg8b loop; the
   helpers below are exactly that loop, so the operations keep the locked
   full-barrier semantics and the exchange-family return value. All of them
   rely on the 8-byte alignment the static asserts above pin. */
static inline uint64_t coro_msx86_exchange(volatile uint64_t* p, uint64_t v) {
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)v, (long long)old);
        if (observed == old) return old;
        old = observed;
    }
}

static inline uint64_t coro_msx86_fetch_add(volatile uint64_t* p, uint64_t v) {
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)(old + v), (long long)old);
        if (observed == old) return old;
        old = observed;
    }
}

static inline uint64_t coro_msx86_or(volatile uint64_t* p, uint64_t v) {
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)(old | v), (long long)old);
        if (observed == old) return old;
        old = observed;
    }
}

static inline uint64_t coro_msx86_and(volatile uint64_t* p, uint64_t v) {
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)(old & v), (long long)old);
        if (observed == old) return old;
        old = observed;
    }
}
#endif /* _M_IX86 */

/* Fast path (x64, default on): a plain volatile access plus a compiler
   barrier. MSVC's volatile keeps its acquire/release meaning and x64
   hardware is TSO, so the load/store pair needs no lock-prefixed
   instruction; the one place where store-load order matters (the Chase-Lev
   singleton race) goes through coro_at_fence below, which stays a full
   fence. Define COROCC_ATOMIC_NO_FASTPATH to fall back to the interlocked
   forms (an A/B measurement switch). MSVC ARM64 is outside the supported
   scope. */
#if defined(_M_X64) && !defined(COROCC_ATOMIC_NO_FASTPATH)

static inline uint64_t coro_at_load(const volatile uint64_t* p) {
    uint64_t v = *p;
    _ReadWriteBarrier();
    return v;
}

static inline void coro_at_store(volatile uint64_t* p, uint64_t v) {
    _ReadWriteBarrier();
    *p = v;
}

#else /* interlocked fallback; also the MSVC non-x64 spellings */

/* MSVC has no plain 64-bit load intrinsic; a compare-exchange against the
   current value both reads it and leaves it untouched. */
static inline uint64_t coro_at_load(const volatile uint64_t* p) {
    return (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
}

static inline void coro_at_store(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    (void)coro_msx86_exchange(p, v);
#else
    _InterlockedExchange64((volatile long long*)p, (long long)v);
#endif
}

#endif /* _M_X64 fast path */

/* Zero-spin accounting (mirrors the GCC path above): every compare-and-swap
   attempt is counted when COROCC_PROBE_NOSPIN is defined, so the zero-spin
   test measures the same fixed-attempt contract on MSVC. */
static inline int coro_at_cas(volatile uint64_t* p, uint64_t* expected,
                              uint64_t desired) {
#ifdef COROCC_PROBE_NOSPIN
    ++coro_probe_cas_count;
#endif
    uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, (long long)desired, (long long)*expected);
    if (observed == *expected) return 1;
    *expected = observed;
    return 0;
}

static inline uint64_t coro_at_fetch_add(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    return coro_msx86_fetch_add(p, v);
#else
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long*)p,
                                               (long long)v);
#endif
}

static inline uint64_t coro_at_exchange(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    return coro_msx86_exchange(p, v);
#else
    return (uint64_t)_InterlockedExchange64((volatile long long*)p,
                                            (long long)v);
#endif
}

static inline uint64_t coro_at_or(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    return coro_msx86_or(p, v);
#else
    return (uint64_t)_InterlockedOr64((volatile long long*)p, (long long)v);
#endif
}

/* Release-OR: see the GCC/Clang note above. The interlocked OR already
   carries release semantics (it is a full barrier), so the same intrinsic
   serves; the x86 helper is the same locked cmpxchg8b loop. */
static inline uint64_t coro_at_or_release(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    return coro_msx86_or(p, v);
#else
    return (uint64_t)_InterlockedOr64((volatile long long*)p, (long long)v);
#endif
}

static inline uint64_t coro_at_and(volatile uint64_t* p, uint64_t v) {
#if defined(_M_IX86)
    return coro_msx86_and(p, v);
#else
    return (uint64_t)_InterlockedAnd64((volatile long long*)p, (long long)v);
#endif
}

static inline void coro_at_fence(void) {
    /* A full fence without <windows.h>: the compiler barrier orders the
       source, the hardware barrier follows. On x64 __faststorefence is the
       documented full barrier (winnt.h's MemoryBarrier expands to the same
       pair); on x86 (MSVC ml/cl, _M_IX86) that intrinsic does not exist and
       _mm_mfence() -- SSE2, the x86 MSVC default -- is the equivalent full
       barrier with StoreLoad included. The x64 codegen was verified
       (fence_probe): lock or dword ptr [rsp],0 -- a full barrier; the x86
       path is verified by the Windows x86 CI job. */
    _ReadWriteBarrier();
#if defined(_M_X64)
    __faststorefence();
#else /* _M_IX86: _mm_mfence */
    _mm_mfence();
#endif
}

/* Sequentially consistent barrier spelled as a zero-delta fetch-add on a
   64-bit word the caller's protocol already owns (see the GCC note above).
   The interlocked add is a full barrier, so the same spelling serves; on
   x86 the zero-delta form is a locked compare-exchange of the value with
   itself (a failed attempt only re-reads; the successful attempt is the
   locked RMW the contract needs). */
static inline void coro_at_fence_rmw(volatile uint64_t* p) {
#if defined(_M_IX86)
    uint64_t old = (uint64_t)_InterlockedCompareExchange64(
        (volatile long long*)p, 0, 0);
    for (;;) {
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)old, (long long)old);
        if (observed == old) break;
        old = observed;
    }
#else
    (void)_InterlockedExchangeAdd64((volatile long long*)p, 0);
#endif
}

static inline uint64_t coro_at_cas_value(volatile uint64_t* p,
                                         uint64_t expected, uint64_t desired) {
    for (;;) {
#ifdef COROCC_PROBE_NOSPIN
        ++coro_probe_cas_count;
#endif
        uint64_t observed = (uint64_t)_InterlockedCompareExchange64(
            (volatile long long*)p, (long long)desired, (long long)expected);
        if (observed == expected) return expected;
        expected = observed;
    }
}

/* Pointer-width counterpart of the family above (see the GCC/Clang section). */
static inline uintptr_t coro_atp_load(const volatile uintptr_t* p) {
    return (uintptr_t)_InterlockedCompareExchangePointer(
        (void* volatile*)p, NULL, NULL);
}

static inline void coro_atp_store(volatile uintptr_t* p, uintptr_t v) {
    _InterlockedExchangePointer((void* volatile*)p, (void*)v);
}

static inline int coro_atp_cas(volatile uintptr_t* p, uintptr_t* expected,
                               uintptr_t desired) {
#ifdef COROCC_PROBE_NOSPIN
    ++coro_probe_cas_count;
#endif
    uintptr_t observed = (uintptr_t)_InterlockedCompareExchangePointer(
        (void* volatile*)p, (void*)desired, (void*)*expected);
    if (observed == *expected) return 1;
    *expected = observed;
    return 0;
}

static inline uintptr_t coro_atp_exchange(volatile uintptr_t* p, uintptr_t v) {
    return (uintptr_t)_InterlockedExchangePointer((void* volatile*)p, (void*)v);
}

#else
#error "corocc: no 64-bit atomic implementation for this compiler (GCC, Clang or MSVC required)"
#endif

#endif /* COROCC_ARCH_ATOMIC_H */
