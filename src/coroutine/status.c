/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_status_str -- implementation of the public status-code stringifier.
 *
 * Kept in its own translation unit so the public header stays free of any
 * implementation detail and the static library always defines every symbol
 * it declares.
 */
#include <corocc/coro_status.h>

const char* coro_status_str(coro_status status) {
    switch (status) {
    case CORO_OK:        return "CORO_OK";
    case CORO_EINVAL:    return "CORO_EINVAL";
    case CORO_ENOMEM:    return "CORO_ENOMEM";
    case CORO_EBUSY:     return "CORO_EBUSY";
    case CORO_ESTATE:    return "CORO_ESTATE";
    case CORO_ECANCELED: return "CORO_ECANCELED";
    case CORO_EDESTROYED:return "CORO_EDESTROYED";
    case CORO_ESTALE:    return "CORO_ESTALE";
    case CORO_EAGAIN:    return "CORO_EAGAIN";
    default:             return "CORO_UNKNOWN";
    }
}
