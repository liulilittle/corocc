/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/ctx.c -- L0 execution context adapter.
 *
 * Implements coro_ctx_init/swap/destroy on top of the vendored Boost.Context
 * assembly (corocc_asm_make / corocc_asm_jump). The assembly only saves and
 * restores registers; all higher-level semantics live here.
 *
 * Entry-return handling: Boost's `finish:` calls _exit, which would terminate
 * the process when a coroutine function returns. To avoid that, every
 * coroutine is entered through our own trampoline `ctx_trampoline`. When the
 * user entry function returns, the trampoline jumps to the *sentinel* context
 * (the resumer) instead of ever reaching `_exit`.
 *
 * Driving convention (verified by probe): the very first jump into a fresh
 * context delivers the jump's second argument (vp) as `transfer_t.data`.
 * `coro_ctx_swap` therefore passes `to` (the target ctx) as vp on the first
 * jump; the trampoline recovers the ctx from t.data and reads the entry /
 * arg / resumer (`back`) stored on it. Nothing is placed inside the coroutine
 * stack where `make_fcontext` carves its register frame, so there is no
 * overlap corruption.
 */
#include <corocc/coro_ctx.h>

#include "arch/fcontext.h"
#include "arch/coro_tsan.h"
#include "arch/coro_tls.h"
#include <stdint.h>
#include <string.h>

#if defined(COROCC_ASAN)
/* ASan fiber-switching hooks (see sanitizer/common_interface_defs.h). Declared
   here to keep the public headers free of any sanitizer dependency. */
void __sanitizer_start_switch_fiber(void** fake_stack_save,
                                    const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                     const void** bottom_old,
                                     size_t* size_old);
#endif

#if defined(COROCC_TSAN)
/* The fiber this thread's own (non-coroutine) code runs on. The driver context
   in coro.c carries no fiber of its own, so a switch that returns to the thread
   resolves to this handle. It is captured on the first swap this thread makes:
   no coroutine can be running before the first swap *into* one, so the first
   swap is always the thread's own code handing control away, which is exactly
   the moment the calling thread is on its own fiber. */
static CORO_TLS void* tsan_thread_fiber;

/* The fiber a context runs on: its own handle for a coroutine, this thread's
   handle for a driver/uninitialised context. */
static void* coro_ctx_tsan_fiber(const coro_ctx* cx) {
    return cx->tsan_fiber ? cx->tsan_fiber : tsan_thread_fiber;
}

/* Announce the switch from `from` to `to` before the assembly jump, so TSan
   attributes the code that will run to the right fiber. */
static void coro_ctx_tsan_switch(coro_ctx* from, coro_ctx* to) {
    void* cur = coro_tsan_current_fiber();
    if (!tsan_thread_fiber) tsan_thread_fiber = cur;
    /* Record the fiber `from` is running on. This is the context the caller is
       suspended in, and the L0 contract lets the caller pass a context it
       never initialised (its fields are only written, then read); recording
       the value here is what gives such a context a valid fiber for the jump
       home that the entry-return trampoline performs. */
    from->tsan_fiber = cur;
    void* dst = to->tsan_fiber ? to->tsan_fiber : tsan_thread_fiber;
    coro_tsan_switch_to_fiber(dst);
}
#endif /* COROCC_TSAN */

/* Entry trampoline: runs the user entry function; when it returns, switches
   to the sentinel (resumer) context. Never reaches Boost's `_exit`. */
static void ctx_trampoline(coro_transfer t) {
    coro_ctx* cx = (coro_ctx*)t.data;
    /* t.fctx is the saved frame of the resumer captured by jump_asm; record
       it into the resumer's ctx so the API invariant "swap fills from->fctx
       with the suspended frame" holds even for the very first swap. */
    cx->back->fctx = t.fctx;
#if defined(COROCC_ASAN)
    /* Finalize the switch the resumer opened before user code runs: an entry
       function may swap again immediately, and ASan rejects a nested start.
       The handle was stashed on this context right before that switch. */
    __sanitizer_finish_switch_fiber(cx->asan_fake, NULL, NULL);
#endif
    cx->entry(cx->arg);
    /* entry returned normally -> back to the resumer/sentinel. Never returns. */
#if defined(COROCC_ASAN)
    {
        /* Leaving this fiber for good: announce the switch back to the
           resumer's stack and hand the handle over, mirroring swap(). */
        const coro_stack* rs = cx->back->stack;
        void* fake = NULL;
        __sanitizer_start_switch_fiber(&fake,
                                       rs ? rs->lo : NULL,
                                       rs ? rs->usable : 0);
        cx->back->asan_fake = fake;
    }
#endif
#if defined(COROCC_TSAN)
    /* The entry returned: the coroutine's fiber is done, so announce the jump
       back to the resumer's fiber (this is the second raw asm_jump site). */
    coro_tsan_switch_to_fiber(coro_ctx_tsan_fiber(cx->back));
#endif
    corocc_asm_jump(cx->back->fctx, NULL);
#if defined(_MSC_VER)
    __assume(0); /* unreachable */
#else
    __builtin_trap(); /* unreachable */
#endif
}

int coro_ctx_init(coro_ctx* cx, const coro_stack* stk,
                  void (*entry)(void*), void* arg) {
    if (!cx || !stk || !entry) return CORO_EINVAL;
    memset(cx, 0, sizeof *cx);
    cx->entry = entry;
    cx->arg   = arg;
    cx->back  = NULL; /* set by the resumer at the first swap */

    uintptr_t hi = (uintptr_t)stk->lo + stk->usable; /* stack top (high addr) */
    /* The PE/MASM backend records the stack bounds from (top, size) into the
       context's NT_TIB fields (StackBase = top, StackLimit = top - size) and
       restores them on every switch, which is what the Windows CRT consults
       when it validates the stack (debug-build GS reporting, _chkstk). A size
       of 0 would leave StackBase == StackLimit (an empty range) and a CRT
       call from inside the coroutine would fail that validation; the SysV
       backend ignores the size argument, so passing the usable size is the
       portable spelling. */
    cx->fctx = corocc_asm_make((void*)hi, stk->usable, ctx_trampoline);
    cx->stack = stk;
#if defined(COROCC_TSAN)
    /* One fiber per context: TSan then follows this coroutine's stack across
       the assembly switches instead of blaming the OS thread that runs it. */
    cx->tsan_fiber = coro_tsan_create_fiber();
#endif
    return CORO_OK;
}

void coro_ctx_swap(coro_ctx* from, coro_ctx* to) {
    if (!from || !to) return;
    /* Remember the resumer so the trampoline can return to it on entry-return.
       Pass `to` as the first-jump data (delivered as transfer_t.data to the
       trampoline). */
    to->back = from;
#if defined(COROCC_ASAN)
    void* fake = NULL;
    /* ASan needs to know the destination stack bounds so it can switch the
       fake stack. A context without a stack (e.g. the main thread) has none
       to register. The handle for the stack we are leaving is stashed on the
       target so the code arriving there can finalize the switch. */
    __sanitizer_start_switch_fiber(
        &fake,
        to->stack ? to->stack->lo : NULL,
        to->stack ? to->stack->usable : 0);
    to->asan_fake = fake;
#endif
#if defined(COROCC_TSAN)
    /* Announce the transfer to the target fiber before the jump. */
    coro_ctx_tsan_switch(from, to);
#endif
    coro_transfer tr = corocc_asm_jump(to->fctx, to);
    /* We are running again, so some context resumed us. tr.fctx is that
       context's saved frame; it identified itself by storing itself in our
       `back` slot immediately before jumping to us, so record the frame
       there. Our own saved frame was handed to whoever we resume above and
       must not be overwritten: a context object survives many swaps, and
       clobbering the slot here would make the next jump land on a stale
       frame. */
    if (tr.data != NULL) {
        from->back->fctx = tr.fctx;
    }
    /* tr.data == NULL: resumed by the entry-return sentinel, which passes no
       resumee identity. The finished coroutine's frame is dead, and our own
       frame was recorded at our first entry, so there is nothing to do. */
#if defined(COROCC_ASAN)
    /* We arrived here because another context switched into us: finalize the
       switch that context opened, using the handle it left on us. */
    __sanitizer_finish_switch_fiber(from->asan_fake, NULL, NULL);
#endif
}

void coro_ctx_destroy(coro_ctx* cx) {
    if (!cx) return;
#if defined(COROCC_TSAN)
    /* The context will not be swapped into again (destroy precondition), so
       its fiber can be retired. A driver/never-initialised context has no
       fiber; coro_tsan_destroy_fiber accepts NULL. */
    coro_tsan_destroy_fiber(cx->tsan_fiber);
#endif
    memset(cx, 0, sizeof *cx);
}