# coro_l0 — status codes, guarded stacks, execution contexts (L0)

> [中文](coro_l0.zh-CN.md) | English

This document is the API and ABI reference for the L0 foundation of corocc:
`coro_status.h` (the result-code type every fallible entry point returns,
plus the `COROCC_API` visibility macro), `coro_stack.h` (the
guard-page-protected coroutine stack) and `coro_ctx.h` (the execution
context: a stack plus a saved register frame). All three headers compile as
C99 and C++11, include no third-party header, and are the layer every higher
level builds on.

The struct layouts below are part of the documented ABI; the measured
`sizeof`/`offsetof` values on every supported pointer model live in
[ABI.md](../ABI.md).

## coro_status — the result code

Every fallible entry point returns a `coro_status`; the library never aborts
on an ordinary error path. A failed call leaves the object's state unchanged
wherever the per-function contract says so, which makes the call safe to
retry or to route to alternate handling. `CORO_OK` is `0`, so `if (status)`
is a valid failure test.

| Value | Meaning |
| --- | --- |
| `CORO_OK` | Success. |
| `CORO_EINVAL` | Invalid argument (NULL, uninitialized object, bad size, out-of-range key). |
| `CORO_ENOMEM` | Out of memory or a backing resource could not be allocated. |
| `CORO_EBUSY` | The operation was rejected because the target is busy (running, full, not drained). |
| `CORO_ESTATE` | The operation is not allowed in the object's current state. |
| `CORO_ECANCELED` | The operation was aborted by a cancellation request (cooperative: observed at a safe point). |
| `CORO_EDESTROYED` | The target has already finished/been destroyed. |
| `CORO_ESTALE` | The referenced credential or generation is stale (for example a resumed-twice token), or the call requires a coroutine on this runtime and was made outside one. |
| `CORO_EAGAIN` | A transient condition; the caller may retry. Also the non-blocking answer of `coro_join` on a live coroutine. |
| `CORO_ECLOSED` | The channel is closed; no further exchange is possible (queued items drain first). |

The numeric values are stable and ordered exactly as listed: the list
position is the value (`CORO_OK == 0` … `CORO_ECLOSED == 9`), and any
future code is appended after the existing ones, never inserted. The
per-entry-point sections below list the subset each call can actually
return.

### `const char* coro_status_str(coro_status status)`

Returns a static, read-only, NUL-terminated string naming the code (for
example `"CORO_OK"`). The pointer is never NULL and stays valid for the
lifetime of the process; it must not be freed or modified. An out-of-range
value maps to a generic `"unknown"` string instead of undefined behavior.

## coro_stack — a guard-page-protected stack

A `coro_stack` owns a contiguous memory region with a hardware guard page at
its low address end. Because stacks grow downward on every supported ABI, an
overflow writes past the usable region into the guard page and faults
instead of silently corrupting neighbouring memory. The type is complete so
callers may embed it by value (on the stack or inside a pool) with no heap
allocation; its fields are private and must be driven only through the API.

```c
typedef struct coro_stack {
    void  *base;        /* private: lowest address of the mapped region (the guard page) */
    size_t total;       /* private: total mapped bytes (guard page + usable) */
    void  *lo;          /* private: lowest usable address (base + guard size) */
    size_t usable;      /* private: usable stack bytes (excluding the guard page) */
    size_t guard_bytes; /* private: size of the guard page, in bytes */
} coro_stack;
```

| Entry point | Contract |
| --- | --- |
| `coro_stack_alloc(stk, size)` | Round `size` up to whole system pages and map that many usable bytes plus one guard page. `size == 0` selects the default. `CORO_OK`; `CORO_EINVAL` for NULL or a size that cannot be served with a guard page; `CORO_ENOMEM` when the page size cannot be determined or the mapping/guard protection fails. On failure `stk` is left zero-initialized and must not be passed to `coro_stack_free`. |
| `coro_stack_free(stk)` | Unmap the whole region and zero-initialize the object so it may be reused or safely freed again. Idempotent: freeing a zero-initialized object is a no-op; NULL is accepted. The stack must not be in use by any context when freed. |
| `coro_stack_size(stk)` | Usable byte count (excluding the guard page), or 0 for NULL / not allocated. |
| `coro_stack_avail(stk)` | Currently available bytes below the usage watermark; an advisory metric (never larger than `coro_stack_size`). 0 for NULL / not allocated. |

Thread-safety: distinct `coro_stack` objects may be allocated and freed
concurrently; a single object must not be used from two threads at once.

## coro_ctx — an execution context

A `coro_ctx` binds an entry function and its argument to a `coro_stack`;
switching between two contexts saves the callee-saved registers of one and
restores those of the other. It performs no system call and holds no lock —
this is the lowest layer of the library.

```c
typedef struct coro_ctx {
    void*             fctx;      /* private: saved register frame (backend-internal) */
    const coro_stack* stack;     /* private: borrowed stack range (sanitizer hooks etc.) */
    void            (*entry)(void*); /* private: coroutine entry function */
    void*             arg;       /* private: opaque argument forwarded to entry */
    struct coro_ctx*  back;      /* private: resumer context (sentinel target) */
    void*             asan_fake; /* private: sanitizer fake-stack save handle; unused otherwise */
} coro_ctx;
```

| Entry point | Contract |
| --- | --- |
| `coro_ctx_init(cx, stk, entry, arg)` | Build the entry frame on a live stack so the first swap into it calls `entry(arg)`. The context borrows `stk` (the stack must outlive the context and stay unchanged). Returns `0` on success and the non-zero `CORO_EINVAL` when `cx`, `stk` or `entry` is NULL. **Note the return type: a plain `int`**, not `coro_status`. Precondition: `cx` is not already live (destroy it first); whether `stk` is currently allocated is likewise a caller precondition, not validated by the call. On failure `cx` is left untouched. |
| `coro_ctx_destroy(cx)` | Release the saved frame and zero the object. Idempotent (NULL accepted); does not free the stack. Precondition: `cx` is not the running context and will not be swapped into again. |
| `coro_ctx_swap(from, to)` | Save the callee-saved state of `from`, restore that of `to`, resume in `to`. Returns "in `from`" the next time some context swaps back. Compile it as nothrow: it must not propagate C++ exceptions across the boundary. Preconditions: both contexts are live and distinct; no lock is held across the call; one thread at a time swaps a given pair. |

## Example

```c
#include <corocc/coro_ctx.h>

static coro_ctx main_ctx, worker;

static void worker_entry(void* arg) {
    (void)arg;
    /* runs on a guard-page-protected stack; returning is safe */
}

int main(void) {
    coro_stack stk;
    if (coro_stack_alloc(&stk, 64 * 1024) != CORO_OK) return 1;
    if (coro_ctx_init(&worker, &stk, worker_entry, NULL) != 0) return 1;

    coro_ctx_swap(&main_ctx, &worker);   /* enter; worker may swap back */
    coro_ctx_swap(&main_ctx, &worker);   /* resume the same context */

    coro_ctx_destroy(&worker);
    coro_stack_free(&stk);
    return 0;
}
```

A context object is reusable: it may be suspended and resumed any number of
times. Returning from the entry function hands control back to the context
that last swapped into it — never to a process exit.

## Guarantees

- **Overflow stays fatal**: the guard page turns a stack overflow into a
  fault instead of silent corruption.
- **Entry return is safe**: returning from the entry function returns control
  to the last resumer; the process never exits.
- **No hidden resource**: a context holds a borrowed stack pointer and a
  saved frame — no system call, no lock, no allocation on the swap path.
- **Portable layout**: every field is a pointer or a `size_t`; the layout is
  identical on LP64 and LLP64 and scales to ILP32 as documented in
  [ABI.md](../ABI.md).

## Thread safety

| Entry point | Thread safety |
| --- | --- |
| `coro_stack_alloc`, `coro_stack_free` | Distinct objects concurrently; one object at a time. |
| `coro_stack_size`, `coro_stack_avail` | Read-only, safe from any thread on a stable object. |
| `coro_ctx_init`, `coro_ctx_destroy` | One thread at a time per context; the context must not be live (or in use) concurrently. |
| `coro_ctx_swap` | One thread at a time per swap pair: a single `coro_ctx` swap pair is not thread-safe by itself. The L1 coroutine layer adds the credential claim that guarantees this for whole coroutines ([coro.md](coro.md)). |
| `coro_status_str` | Pure; safe from any thread. |
