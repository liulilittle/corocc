#ifndef COROCC_ARCH_TLS_H
#define COROCC_ARCH_TLS_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/arch/coro_tls.h -- internal thread-local storage qualifier.
 *
 * Used for the single per-thread "currently running coroutine" pointer the
 * library keeps. C99 has no thread-local storage keyword, so the compiler's
 * own spelling is selected here. MSVC's __declspec(thread) is safe for the
 * dynamically loaded case on modern toolchains; the Windows backend is
 * exercised on the Windows CI job.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define CORO_TLS __thread
#elif defined(_MSC_VER)
#  define CORO_TLS __declspec(thread)
#else
#  error "corocc: no thread-local storage qualifier for this compiler (GCC, Clang or MSVC required)"
#endif

#endif /* COROCC_ARCH_TLS_H */
