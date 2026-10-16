#ifndef COROCC_FCONTEXT_H
#define COROCC_FCONTEXT_H

/*
 * Boost.Context fcontext declarations, renamed for corocc.
 * The `coro_fctx` / `coro_transfer` types and the function prototypes are
 * Boost's `fcontext_t` / `transfer_t` ABI (see THIRD-PARTY.md), declared here
 * so the vendored assembly can be driven without any Boost header.
 *
 * Register convention (fixed by the vendored asm, not invented here):
 *   - corocc_asm_make returns a context in the platform's first return
 *     register;
 *   - corocc_asm_jump returns the transfer_t as the platform's standard
 *     two-pointer aggregate result.
 *
 * Mach-O (macOS) selection branch: an explicit per-arch record of the
 * register pair the vendored macho backends deliver. The C prototypes below
 * are identical on every backend -- the compiler's aggregate-return ABI
 * matches what the assembly produces.
 */
#if defined(__APPLE__) && defined(__x86_64__)
/* macOS x86_64 (x86_64_sysv_macho_gas.S): fctx in RAX, data in RDX. */
#elif defined(__APPLE__) && defined(__aarch64__)
/* macOS arm64 (arm64_aapcs_macho_gas.S): fctx in X0, data in X1. */
#elif defined(_M_ARM64)
/* Windows ARM64 (arm64_ms.asm): fctx in X0, data in X1. */
#elif defined(__x86_64__)
/* x86_64 SysV ELF (x86_64_sysv_elf_gas.S): fctx in RAX, data in RDX. */
#elif defined(__aarch64__)
/* aarch64 AAPCS ELF (arm64_aapcs_elf_gas.S): fctx in X0, data in X1. */
#elif defined(_M_IX86)
/* Win32 MSVC (i386_ms.asm): the pair returns in EDX:EAX, fctx in EAX. */
#else
/* Other vendored ELF backends: the pair is returned in that ABI's standard
   two-register aggregate slots, matched by the C prototype below. */
#endif
#include <stddef.h>

typedef void* coro_fctx;
typedef struct coro_transfer { coro_fctx fctx; void* data; } coro_transfer;

extern coro_fctx     corocc_asm_make(void* sp, size_t size, void (*fn)(coro_transfer));
extern coro_transfer corocc_asm_jump(coro_fctx to, void* vp);

#endif /* COROCC_FCONTEXT_H */
