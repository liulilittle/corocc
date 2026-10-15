/*
 * Vendored from Boost.Context 1.83.0
 *   src/asm/make_ppc32_sysv_elf_gas.S
 *   src/asm/jump_ppc32_sysv_elf_gas.S
 * Distributed under the Boost Software License, Version 1.0.
 * See THIRD-PARTY.md for origin, version and license.
 *
 * Only the public symbol names were renamed so that corocc can coexist in one
 * binary with Boost:
 *   make_fcontext -> corocc_asm_make
 *   jump_fcontext -> corocc_asm_jump
 * The machine code is unchanged; the original copyright headers are kept
 * verbatim.
 */

/* --- begin make_ppc32_sysv_elf_gas.S (renamed corocc_asm_make) --- */
/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  4  |  8  |  12 |  16 |  20 |  24 |  28 |  *
 *  -------------------------------------------------  *
 *  |bchai|hiddn|   fpscr   |  PC |  CR | R14 | R15 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  36 |  40 |  44 |  48 |  52 |  56 |  60 |  *
 *  -------------------------------------------------  *
 *  | R16 | R17 | R18 | R19 | R20 | R21 | R22 | R23 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  64 |  68 |  72 |  76 |  80 |  84 |  88 |  92 |  *
 *  -------------------------------------------------  *
 *  | R24 | R25 | R26 | R27 | R28 | R29 | R30 | R31 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 |  *
 *  -------------------------------------------------  *
 *  |    F14    |    F15    |    F16    |    F17    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 |  *
 *  -------------------------------------------------  *
 *  |    F18    |    F19    |    F20    |    F21    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 160 | 164 | 168 | 172 | 176 | 180 | 184 | 188 |  *
 *  -------------------------------------------------  *
 *  |    F22    |    F23    |    F24    |    F25    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 192 | 196 | 200 | 204 | 208 | 212 | 216 | 220 |  *
 *  -------------------------------------------------  *
 *  |    F26    |    F27    |    F28    |    F29    |  *
 *  -------------------------------------------------  *
 *  ------------------------|------------              *
 *  | 224 | 228 | 232 | 236 | 240 | 244 |              *
 *  ------------------------|------------              *
 *  |    F30    |    F31    |bchai|  LR |              *
 *  ------------------------|------------              *
 *                                                     *
 *******************************************************/

.file "make_ppc32_sysv_elf_gas.S"
.text
.globl corocc_asm_make
.align 2
.type corocc_asm_make,@function
corocc_asm_make:
    # save return address into R6
    mflr  %r6

    # first arg of corocc_asm_make() == top address of context-function
    # shift address in R3 to lower 16 byte boundary
    clrrwi  %r3, %r3, 4

    # reserve space on context-stack, including 16 bytes of linkage
    # and parameter area + 240 bytes of context-data (R1 % 16 == 0)
    subi  %r3, %r3, 16 + 240

    # third arg of corocc_asm_make() == address of context-function
#ifdef __linux__
    # save context-function as PC
    stw  %r5, 16(%r3)
#else
    # save context-function for trampoline
    stw  %r5, 248(%r3)
#endif

    # set back-chain to zero
    li   %r0, 0
    stw  %r0, 240(%r3)

    # copy FPSCR to new context
    mffs  %f0
    stfd  %f0, 8(%r3)

#ifdef __linux__
    # set hidden pointer for returning transfer_t
    la    %r0, 248(%r3)
    stw   %r0, 4(%r3)
#endif

    # load address of label 1 into R4
    bl  1f
1:  mflr  %r4
#ifndef __linux__
    # compute abs address of trampoline, use as PC
    addi  %r7, %r4, trampoline - 1b
    stw   %r7, 16(%r3)
#endif
    # compute abs address of label finish
    addi  %r4, %r4, finish - 1b
    # save address of finish as return-address for context-function
    # will be entered after context-function returns
    stw  %r4, 244(%r3)

    # restore return address from R6
    mtlr  %r6

    blr  # return pointer to context-data

#ifndef __linux__
trampoline:
    # On systems other than Linux, jump_fcontext is returning the
    # transfer_t in R3:R4, but we need to pass transfer_t * R3 to
    # our context-function.
    lwz   %r0, 8(%r1)   # address of context-function
    mtctr %r0
    stw   %r3, 8(%r1)
    stw   %r4, 12(%r1)
    la    %r3, 8(%r1)   # address of transfer_t
    bctr
#endif

finish:
    # Use the secure PLT for _exit(0).  If we use the insecure BSS PLT
    # here, then the linker may use the insecure BSS PLT even if the
    # C++ compiler wanted the secure PLT.

    # set R30 for secure PLT, large model
    bl     2f
2:  mflr   %r30
    addis  %r30, %r30, .Ltoc - 2b@ha
    addi   %r30, %r30, .Ltoc - 2b@l

    # call _exit(0) with special addend 0x8000 for large model
    li  %r3, 0
    bl  _exit + 0x8000@plt
.size corocc_asm_make, .-corocc_asm_make

/* Provide the GOT pointer for secure PLT, large model. */
.section .got2,"aw"
.Ltoc = . + 0x8000

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits

/* --- begin jump_ppc32_sysv_elf_gas.S (renamed corocc_asm_jump) --- */
/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  4  |  8  |  12 |  16 |  20 |  24 |  28 |  *
 *  -------------------------------------------------  *
 *  |bchai|hiddn|   fpscr   |  PC |  CR | R14 | R15 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  36 |  40 |  44 |  48 |  52 |  56 |  60 |  *
 *  -------------------------------------------------  *
 *  | R16 | R17 | R18 | R19 | R20 | R21 | R22 | R23 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  64 |  68 |  72 |  76 |  80 |  84 |  88 |  92 |  *
 *  -------------------------------------------------  *
 *  | R24 | R25 | R26 | R27 | R28 | R29 | R30 | R31 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 |  *
 *  -------------------------------------------------  *
 *  |    F14    |    F15    |    F16    |    F17    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 |  *
 *  -------------------------------------------------  *
 *  |    F18    |    F19    |    F20    |    F21    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 160 | 164 | 168 | 172 | 176 | 180 | 184 | 188 |  *
 *  -------------------------------------------------  *
 *  |    F22    |    F23    |    F24    |    F25    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  | 192 | 196 | 200 | 204 | 208 | 212 | 216 | 220 |  *
 *  -------------------------------------------------  *
 *  |    F26    |    F27    |    F28    |    F29    |  *
 *  -------------------------------------------------  *
 *  ------------------------|------------              *
 *  | 224 | 228 | 232 | 236 | 240 | 244 |              *
 *  ------------------------|------------              *
 *  |    F30    |    F31    |bchai|  LR |              *
 *  ------------------------|------------              *
 *                                                     *
 *******************************************************/

.file "jump_ppc32_sysv_elf_gas.S"
.text
.globl corocc_asm_jump
.align 2
.type corocc_asm_jump,@function
corocc_asm_jump:
    # Linux: corocc_asm_jump( hidden transfer_t * R3, R4, R5)
    # Other: transfer_t R3:R4 = corocc_asm_jump( R3, R4)

    mflr  %r0  # return address from LR
    mffs  %f0  # FPSCR
    mfcr  %r8  # condition register

    stwu  %r1, -240(%r1)  # allocate stack space, R1 % 16 == 0
    stw   %r0, 244(%r1)   # save LR in caller's frame

#ifdef __linux__
    stw   %r3, 4(%r1)   # hidden pointer
#endif

    stfd  %f0, 8(%r1)   # FPSCR
    stw   %r0, 16(%r1)  # LR as PC
    stw   %r8, 20(%r1)  # CR

    # Save registers R14 to R31.
    # Don't change R2, the thread-local storage pointer.
    # Don't change R13, the small data pointer.
    stw   %r14, 24(%r1)
    stw   %r15, 28(%r1)
    stw   %r16, 32(%r1)
    stw   %r17, 36(%r1)
    stw   %r18, 40(%r1)
    stw   %r19, 44(%r1)
    stw   %r20, 48(%r1)
    stw   %r21, 52(%r1)
    stw   %r22, 56(%r1)
    stw   %r23, 60(%r1)
    stw   %r24, 64(%r1)
    stw   %r25, 68(%r1)
    stw   %r26, 72(%r1)
    stw   %r27, 76(%r1)
    stw   %r28, 80(%r1)
    stw   %r29, 84(%r1)
    stw   %r30, 88(%r1)
    stw   %r31, 92(%r1)

    # Save registers F14 to F31 in slots with 8-byte alignment.
    # 4-byte alignment may stall the pipeline of some processors.
    # Less than 4 may cause alignment traps.
    stfd  %f14, 96(%r1)
    stfd  %f15, 104(%r1)
    stfd  %f16, 112(%r1)
    stfd  %f17, 120(%r1)
    stfd  %f18, 128(%r1)
    stfd  %f19, 136(%r1)
    stfd  %f20, 144(%r1)
    stfd  %f21, 152(%r1)
    stfd  %f22, 160(%r1)
    stfd  %f23, 168(%r1)
    stfd  %f24, 176(%r1)
    stfd  %f25, 184(%r1)
    stfd  %f26, 192(%r1)
    stfd  %f27, 200(%r1)
    stfd  %f28, 208(%r1)
    stfd  %f29, 216(%r1)
    stfd  %f30, 224(%r1)
    stfd  %f31, 232(%r1)

    # store RSP (pointing to context-data) in R7/R6
    # restore RSP (pointing to context-data) from R4/R3
#ifdef __linux__
    mr   %r7, %r1
    mr   %r1, %r4
    lwz  %r3, 4(%r1)   # hidden pointer
#else
    mr   %r6, %r1
    mr   %r1, %r3
#endif

    lfd  %f0, 8(%r1)   # FPSCR
    lwz  %r0, 16(%r1)  # PC
    lwz  %r8, 20(%r1)  # CR

    mtfsf  0xff, %f0   # restore FPSCR
    mtctr  %r0         # load CTR with PC
    mtcr   %r8         # restore CR

    # restore R14 to R31
    lwz  %r14, 24(%r1)
    lwz  %r15, 28(%r1)
    lwz  %r16, 32(%r1)
    lwz  %r17, 36(%r1)
    lwz  %r18, 40(%r1)
    lwz  %r19, 44(%r1)
    lwz  %r20, 48(%r1)
    lwz  %r21, 52(%r1)
    lwz  %r22, 56(%r1)
    lwz  %r23, 60(%r1)
    lwz  %r24, 64(%r1)
    lwz  %r25, 68(%r1)
    lwz  %r26, 72(%r1)
    lwz  %r27, 76(%r1)
    lwz  %r28, 80(%r1)
    lwz  %r29, 84(%r1)
    lwz  %r30, 88(%r1)
    lwz  %r31, 92(%r1)

    # restore F14 to F31
    lfd  %f14, 96(%r1)
    lfd  %f15, 104(%r1)
    lfd  %f16, 112(%r1)
    lfd  %f17, 120(%r1)
    lfd  %f18, 128(%r1)
    lfd  %f19, 136(%r1)
    lfd  %f20, 144(%r1)
    lfd  %f21, 152(%r1)
    lfd  %f22, 160(%r1)
    lfd  %f23, 168(%r1)
    lfd  %f24, 176(%r1)
    lfd  %f25, 184(%r1)
    lfd  %f26, 192(%r1)
    lfd  %f27, 200(%r1)
    lfd  %f28, 208(%r1)
    lfd  %f29, 216(%r1)
    lfd  %f30, 224(%r1)
    lfd  %f31, 232(%r1)

    # restore LR from caller's frame
    lwz   %r0, 244(%r1)
    mtlr  %r0

    # adjust stack
    addi  %r1, %r1, 240

    # return transfer_t
#ifdef __linux__
    stw  %r7, 0(%r3)
    stw  %r5, 4(%r3)
#else
    mr   %r3, %r6
    #    %r4, %r4
#endif

    # jump to context
    bctr
.size corocc_asm_jump, .-corocc_asm_jump

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits
