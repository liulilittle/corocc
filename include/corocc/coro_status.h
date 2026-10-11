/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_status.h -- public status codes and the export macro.
 *
 * This header is the root of the public API surface: it defines
 * COROCC_API (used by every other public header) and the coro_status
 * enumeration returned by fallible entry points. It includes nothing
 * beyond the C standard library so that it compiles as both C99 and
 * C++11 and never drags in a Boost (or any third-party) header.
 */
#ifndef COROCC_CORO_STATUS_H
#define COROCC_CORO_STATUS_H

/*
 * COROCC_API -- linkage/visibility attribute for every public symbol.
 *
 * On Windows builds that produce a DLL, COROCC_BUILDING must be defined
 * exactly once (by the shared-library translation unit) to export the
 * symbols; consumers then see them as dllimport. On ELF/Mach-O toolchains
 * the attribute requests default visibility so the symbol stays exported
 * even though the library itself is compiled with -fvisibility=hidden.
 */
#if defined(_WIN32)
#  if defined(COROCC_BUILDING)
#    define COROCC_API __declspec(dllexport)
#  else
#    define COROCC_API __declspec(dllimport)
#  endif
#else
#  define COROCC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * coro_status -- result code returned by every fallible corocc entry point.
 *
 * corocc never aborts on an ordinary error path; it reports failures via
 * this enumeration so callers can recover deterministically. A failed call
 * leaves the object's state unchanged wherever the contract documents it,
 * making the call safe to retry or to route to alternate handling.
 *
 * Values are stable and ordered: CORO_OK is 0 so that `if (status)` is a
 * valid failure test.
 */
typedef enum {
    CORO_OK = 0,      /**< Success. */
    CORO_EINVAL,      /**< Invalid argument (NULL, uninitialized, bad size). */
    CORO_ENOMEM,      /**< Out of memory / backing resource allocation failed. */
    CORO_EBUSY,       /**< Operation rejected because the target is busy. */
    CORO_ESTATE,      /**< Operation not allowed in the current state. */
    CORO_ECANCELED,   /**< Operation aborted by cancellation. */
    CORO_EDESTROYED,  /**< Target was already destroyed. */
    CORO_ESTALE,      /**< Referenced credential/generation is stale. */
    CORO_EAGAIN       /**< Transient condition; caller may retry. */
} coro_status;

/**
 * coro_status_str -- human-readable, static string for a status code.
 *
 * @param status  Status value to describe.
 * @return A pointer to a static, read-only, NUL-terminated string naming
 *         the code (e.g. "CORO_OK"). The pointer is never NULL and remains
 *         valid for the lifetime of the process; it must not be freed or
 *         modified. An out-of-range value maps to a generic "unknown"
 *         string rather than causing undefined behavior.
 */
COROCC_API const char* coro_status_str(coro_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_STATUS_H */
