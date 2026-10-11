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

/* Entry trampoline: runs the user entry function; when it returns, switches
   to the sentinel (resumer) context. Never reaches Boost's `_exit`. */
static void ctx_trampoline(coro_transfer t) {
    coro_ctx* cx = (coro_ctx*)t.data;
    /* t.fctx is the saved frame of the resumer captured by jump_asm; record
       it into the resumer's ctx so the API invariant "swap fills from->fctx
       with the suspended frame" holds even for the very first swap. */
    cx->back->fctx = t.fctx;
    cx->entry(cx->arg);
    /* entry returned normally -> back to the resumer/sentinel. Never returns. */
    corocc_asm_jump(cx->back->fctx, NULL);
    __builtin_trap(); /* unreachable */
}

int coro_ctx_init(coro_ctx* cx, const coro_stack* stk,
                  void (*entry)(void*), void* arg) {
    if (!cx || !stk || !entry) return CORO_EINVAL;
    memset(cx, 0, sizeof *cx);
    cx->entry = entry;
    cx->arg   = arg;
    cx->back  = NULL; /* set by the resumer at the first swap */

    uintptr_t hi = (uintptr_t)stk->lo + stk->usable; /* stack top (high addr) */
    cx->fctx = corocc_asm_make((void*)hi, 0, ctx_trampoline);
    cx->stack = stk;
    return CORO_OK;
}

void coro_ctx_swap(coro_ctx* from, coro_ctx* to) {
    if (!from || !to) return;
    /* Remember the resumer so the trampoline can return to it. Pass `to` as
       the first-jump data (delivered as transfer_t.data to the trampoline). */
    to->back = from;
#if defined(COROCC_ASAN)
    void* fake = NULL;
    /* ASan needs to know the target stack so it can switch the fake stack.
       A context without a stack (e.g. the main thread) has none to register. */
    __sanitizer_start_switch_fiber(
        &fake,
        to->stack ? to->stack->lo : NULL,
        to->stack ? to->stack->usable : 0);
    {
        coro_transfer tr = corocc_asm_jump(to->fctx, to);
        from->fctx = tr.fctx;
    }
    {
        const void* lo_from = from->stack ? from->stack->lo : NULL;
        size_t      us_from = from->stack ? from->stack->usable : 0;
        __sanitizer_finish_switch_fiber(fake, &lo_from, &us_from);
    }
#else
    coro_transfer tr = corocc_asm_jump(to->fctx, to);
    from->fctx = tr.fctx;
#endif
}

void coro_ctx_destroy(coro_ctx* cx) {
    if (cx) memset(cx, 0, sizeof *cx);
}