;      Copyright Edward Nevill + Oliver Kowalke 2015
;  Distributed under the Boost Software License, Version 1.0.
;     (See accompanying file LICENSE_1_0.txt or copy at
;         http://www.boost.org/LICENSE_1_0.txt)
;
; Vendored from Boost.Context 1.92.0 (tag boost-1.92.0, commit
; cc1b9e02cd530d6607becc984e1e5bfa4f7b3600)
;   src/asm/make_arm64_aapcs_pe_armasm.asm
;   src/asm/jump_arm64_aapcs_pe_armasm.asm
; Distributed under the Boost Software License, Version 1.0.
; See THIRD-PARTY.md for origin, version and license.
;
; Only the public symbol names were renamed so that corocc can coexist in one
; binary with Boost:
;   make_fcontext -> corocc_asm_make
;   jump_fcontext -> corocc_asm_jump
; The machine code is unchanged; the original copyright header above is kept
; verbatim. Windows ARM64 symbol decoration adds nothing (the MSVC ARM64 C
; name is the symbol name), so the exported symbols are corocc_asm_make /
; corocc_asm_jump; the imported CRT symbol is _exit.
;
; ABI note: the backend carries the Windows TIB fields through the context
; (stack base/limit, deallocation stack, fiber data, read and written via the
; x18 platform register at the Windows ARM64 TIB offsets, see ksarm64.h), and
; jump returns the transfer_t in X0/X1 -- the ARM64 two-register aggregate
; result, matching the C prototypes in src/arch/fcontext.h. The area is built
; by armasm64 through CMake's ASM_MARMASM language.

;*******************************************************
;*                                                     *
;*  -------------------------------------------------  *
;*  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
;*  -------------------------------------------------  *
;*  | 0x0 | 0x4 | 0x8 | 0xc | 0x10| 0x14| 0x18| 0x1c|  *
;*  -------------------------------------------------  *
;*  |    d8     |    d9     |    d10    |    d11    |  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
;*  -------------------------------------------------  *
;*  | 0x20| 0x24| 0x28| 0x2c| 0x30| 0x34| 0x38| 0x3c|  *
;*  -------------------------------------------------  *
;*  |    d12    |    d13    |    d14    |    d15    |  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
;*  -------------------------------------------------  *
;*  | 0x40| 0x44| 0x48| 0x4c| 0x50| 0x54| 0x58| 0x5c|  *
;*  -------------------------------------------------  *
;*  |    x19    |    x20    |    x21    |    x22    |  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  24 |  25 |  26 |  27 |  28 |  29 |  30 |  31 |  *
;*  -------------------------------------------------  *
;*  | 0x60| 0x64| 0x68| 0x6c| 0x70| 0x74| 0x78| 0x7c|  *
;*  -------------------------------------------------  *
;*  |    x23    |    x24    |    x25    |    x26    |  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
;*  -------------------------------------------------  *
;*  | 0x80| 0x84| 0x88| 0x8c| 0x90| 0x94| 0x98| 0x9c|  *
;*  -------------------------------------------------  *
;*  |    x27    |    x28    |    FP     |     LR    |  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  40 |  41 |  42 |  43 |  44 |  45 |  46 |  47 |  *
;*  -------------------------------------------------  *
;*  | 0xa0| 0xa4| 0xa8| 0xac| 0xb0| 0xb4| 0xb8| 0xbc|  *
;*  -------------------------------------------------  *
;*  |   base    |   limit   |  dealloc  | fiber data|  *
;*  -------------------------------------------------  *
;*  -------------------------------------------------  *
;*  |  48 |  49 |  50 | 51  |           |           |  *
;*  -------------------------------------------------  *
;*  | 0xc0| 0xc4| 0xc8| 0xcc|           |           |  *
;*  -------------------------------------------------  *
;*  |     PC    |   align   |           |           |  *
;*  -------------------------------------------------  *
;*                                                     *
;*******************************************************

    AREA |.text|, CODE, READONLY, ALIGN=4, CODEALIGN
    EXPORT corocc_asm_make
    IMPORT _exit

corocc_asm_make proc
    ; save stack top address to x3
    mov x3, x0

    ; shift address in x0 (allocated stack) to lower 16 byte boundary
    and x0, x0, ~0xF

    ; reserve space for context-data on context-stack
    sub  x0, x0, #0xd0

    ; save top address of context_stack as 'base'
    str  x3, [x0, #0xa0]
    ; save bottom address of context-stack as 'limit' and 'deallocation stack'
    sub  x3, x3, x1
    stp  x3, x3, [x0, #0xa8]
    ; save 0 as 'fiber data'
    str  xzr, [x0, #0xb8]

    ; third arg of make_fcontext() == address of context-function
    ; store address as x19 for trampoline
    str  x2, [x0, #0x40]
    ; store trampoline address as pc
    adr  x2, trampoline
    str  x2, [x0, #0xc0]

    ; save address of finish as return-address for context-function
    ; will be entered after context-function returns (LR register)
    adr  x1, finish
    str  x1, [x0, #0x98]

    ret  x30 ; return pointer to context-data (x0)

trampoline
    stp  fp, lr, [sp, #-0x10]!
    mov  fp, sp
    blr x19

finish
    ; exit code is zero
    mov  x0, #0
    ; exit application
    bl  _exit
    ENDP

; --- begin jump_arm64_aapcs_pe_armasm.asm ---

    EXPORT corocc_asm_jump

corocc_asm_jump proc
    ; prepare stack for GP + FPU
    sub  sp, sp, #0xd0

    ; save d8 - d15
    stp  d8,  d9,  [sp, #0x00]
    stp  d10, d11, [sp, #0x10]
    stp  d12, d13, [sp, #0x20]
    stp  d14, d15, [sp, #0x30]

    ; save x19-x30
    stp  x19, x20, [sp, #0x40]
    stp  x21, x22, [sp, #0x50]
    stp  x23, x24, [sp, #0x60]
    stp  x25, x26, [sp, #0x70]
    stp  x27, x28, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]

    ; save LR as PC
    str  x30, [sp, #0xc0]

    ; save current stack base and limit
    ldp  x5,  x6,  [x18, #0x08] ; TeStackBase and TeStackLimit at ksarm64.h
    stp  x5,  x6,  [sp, #0xa0]
    ; save current fiber data and deallocation stack
    ldr  x5, [x18, #0x1478] ; TeDeallocationStack at ksarm64.h
    ldr  x6, [x18, #0x20] ; TeFiberData at ksarm64.h
    stp  x5,  x6,  [sp, #0xb0]

    ; store RSP (pointing to context-data) in X0
    mov  x4, sp

    ; restore RSP (pointing to context-data) from X1
    mov  sp, x0

    ; restore stack base and limit
    ldp  x5,  x6,  [sp, #0xa0]
    stp  x5,  x6,  [x18, #0x08] ; TeStackBase and TeStackLimit at ksarm64.h
    ; restore fiber data and deallocation stack
    ldp  x5,  x6,  [sp, #0xb0]
    str  x5, [x18, #0x1478] ; TeDeallocationStack at ksarm64.h
    str  x6, [x18, #0x20] ; TeFiberData at ksarm64.h

    ; load d8 - d15
    ldp  d8,  d9,  [sp, #0x00]
    ldp  d10, d11, [sp, #0x10]
    ldp  d12, d13, [sp, #0x20]
    ldp  d14, d15, [sp, #0x30]

    ; load x19-x30
    ldp  x19, x20, [sp, #0x40]
    ldp  x21, x22, [sp, #0x50]
    ldp  x23, x24, [sp, #0x60]
    ldp  x25, x26, [sp, #0x70]
    ldp  x27, x28, [sp, #0x80]
    ldp  x29, x30, [sp, #0x90]

    ; return transfer_t from jump
    ; pass transfer_t as first arg in context function
    ; X0 == FCTX, X1 == DATA
    mov x0, x4

    ; load pc
    ldr  x4, [sp, #0xc0]

    ; restore stack from GP + FPU
    add  sp, sp, #0xd0

    ret x4
    ENDP
    END
