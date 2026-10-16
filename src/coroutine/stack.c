/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/stack.c -- guarded stack allocation.
 *
 * A coroutine stack is a contiguous mapping of `usable` bytes plus one
 * guard page placed at the *low* address end. Stacks grow downward on
 * every ABI we support, so an overflow writes below `lo` and lands in the
 * guard page, producing a diagnosable fault -- SIGSEGV on POSIX, an
 * access violation (0xC0000005) on Windows -- instead of silently
 * corrupting neighbouring memory. The POSIX branch maps with
 * mmap/mprotect; the Windows branch reserves with VirtualAlloc and turns
 * the low page into PAGE_NOACCESS with VirtualProtect. Both branches fill
 * the same five fields with the same meaning.
 *
 * `struct coro_stack` is a complete public type defined in
 * <corocc/coro_stack.h>; it is never redefined here (no internal.h).
 */
#include <corocc/coro_stack.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
/* Darwin spells the anonymous mapping flag MAP_ANON (sys/mman.h);
   MAP_ANONYMOUS exists only on the other POSIX systems this library builds
   on (Linux/glibc). Alias it so the mapping call below has one spelling. */
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#include <stdint.h>
#include <string.h>

/* Default usable stack size when the caller passes 0. */
#define COROCC_DEFAULT_STACK_BYTES (256u * 1024u)

/**
 * coro_stack_alloc -- map a stack region with a low-address guard page.
 *
 * See coro_stack.h for the full contract. `size` is rounded up to a whole
 * number of pages; at least two usable pages are required so that the guard
 * page plus a minimally useful stack exist.
 */
coro_status coro_stack_alloc(coro_stack* s, size_t size) {
    if (!s) return CORO_EINVAL;
    memset(s, 0, sizeof *s);

    if (size == 0) size = COROCC_DEFAULT_STACK_BYTES;

#if defined(_WIN32)
    {
        SYSTEM_INFO si;
        size_t guard;
        size_t usable;
        size_t total;
        void* base;
        DWORD old_protect = 0;

        GetSystemInfo(&si);
        if (si.dwPageSize == 0) return CORO_ENOMEM;
        guard = (size_t)si.dwPageSize;
        usable = (size + guard - 1) & ~(guard - 1);
        if (usable < guard * 2u) return CORO_EINVAL; /* too small: need >= 2 pages usable */

        total = usable + guard;
        base = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE);
        if (!base) return CORO_ENOMEM;

        /* The guard page sits at the low address end; the stack grows down
           into it. */
        if (!VirtualProtect(base, guard, PAGE_NOACCESS, &old_protect)) {
            VirtualFree(base, 0, MEM_RELEASE);
            return CORO_ENOMEM;
        }

        s->base = base;
        s->total = total;
        s->lo = (char*)base + guard;
        s->usable = usable;
        s->guard_bytes = guard;
        return CORO_OK;
    }
#else
    {
        long ps = sysconf(_SC_PAGESIZE);
        size_t guard;
        size_t usable;
        size_t total;
        void* base;

        if (ps <= 0) return CORO_ENOMEM;
        guard = (size_t)ps;
        usable = (size + guard - 1) & ~(guard - 1);
        if (usable < guard * 2u) return CORO_EINVAL; /* too small: need >= 2 pages usable */

        total = usable + guard;
        base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) return CORO_ENOMEM;

        if (mprotect(base, guard, PROT_NONE) != 0) {
            munmap(base, total);
            return CORO_ENOMEM;
        }

        s->base = base;
        s->total = total;
        s->lo = (char*)base + guard;
        s->usable = usable;
        s->guard_bytes = guard;
        return CORO_OK;
    }
#endif
}

/**
 * coro_stack_free -- unmap a stack region and reset the object.
 *
 * Idempotent: a NULL pointer or an already-freed (zeroed) object is a no-op.
 */
void coro_stack_free(coro_stack* s) {
    if (!s || !s->base) return;
#if defined(_WIN32)
    VirtualFree(s->base, 0, MEM_RELEASE);
#else
    munmap(s->base, s->total);
#endif
    memset(s, 0, sizeof *s);
}

/** coro_stack_size -- usable byte count, or 0 if unallocated. */
size_t coro_stack_size(const coro_stack* s) {
    return s ? s->usable : 0;
}

/** coro_stack_avail -- available bytes; full usable size (no fine watermark yet). */
size_t coro_stack_avail(const coro_stack* s) {
    return s ? s->usable : 0;
}
