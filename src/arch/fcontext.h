#ifndef COROCC_FCONTEXT_H
#define COROCC_FCONTEXT_H

/*
 * Boost.Context x86_64 SysV ABI declarations, renamed for corocc.
 * The `coro_fctx` / `coro_transfer` types and the function prototypes are
 * Boost's `fcontext_t` / `transfer_t` ABI (see THIRD-PARTY.md), declared here
 * so the vendored assembly can be driven without any Boost header.
 *
 * Register convention (fixed by the vendored asm, not invented here):
 *   - corocc_asm_make returns a context in RAX;
 *   - corocc_asm_jump returns the transfer_t in RAX (%rax == fctx) and
 *     RDX (%rdx == data).
 */
#include <stddef.h>

typedef void* coro_fctx;
typedef struct coro_transfer { coro_fctx fctx; void* data; } coro_transfer;

extern coro_fctx     corocc_asm_make(void* sp, size_t size, void (*fn)(coro_transfer));
extern coro_transfer corocc_asm_jump(coro_fctx to, void* vp);

#endif /* COROCC_FCONTEXT_H */
