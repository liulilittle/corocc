/*
 * mips N32 context-switch backend (big/little endian, ILP32 data model on
 * 64-bit registers). Hand-written for W6, following the calling conventions
 * proven by the vendored mips32_o32 / mips64_n64 backends in this directory:
 *   - N32 keeps 64-bit general registers, so the s0-s7/fp/ra/gp/pc slots are
 *     8-byte wide (sd/ld), unlike the 4-byte o32 slots;
 *   - transfer_t (8 bytes) is returned in $v0/$v1, exactly like n64 - the
 *     calibration probe ("jal f" with no hidden $a0 setup) confirmed it for
 *     -mabi=n32 with gcc-13;
 *   - FP saves use the even f20-f30 double set (sdc1, 6 x 8 bytes), which the
 *     same probe confirmed as the callee-saved FP set for this toolchain.
 * Public symbols follow the repository convention:
 *   corocc_asm_make / corocc_asm_jump
 */

/* --- make_fcontext (renamed corocc_asm_make) --- */
/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |   0    |   8    |   16   |   24   |   32   | 40  |  *
 *  -------------------------------------------------  *
 *  |   F20  |   F22  |   F24  |   F26  |   F28  | F30 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |   48   |   56   |   64   |   72   |  80   | 88  |  *
 *  -------------------------------------------------  *
 *  |   S0   |   S1   |   S2   |   S3   |   S4   | S5  |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |   96   |  104   |  112   |  120   |  128  | 136 |  *
 *  -------------------------------------------------  *
 *  |   S6   |   S7   |   FP   |   GP   |   RA  | PC  |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "mips32_n32_elf_gas.S"
.text
.globl corocc_asm_make
.align 2
.type corocc_asm_make,@function
.ent corocc_asm_make
corocc_asm_make:
#ifdef __PIC__
.set    noreorder
.cpload $t9
.set    reorder
#endif
    # shift address in A0 to lower 16 byte boundary
    li $v1, -16 # 0xfffffffffffffff0
    and $v0, $v1, $a0

    # reserve space for context-data on context-stack
    addiu $v0, $v0, -144

    # third arg of corocc_asm_make() == address of context-function
    # store address as a PC to jump in
    sd  $a2, 136($v0)
    # save global pointer in context-data
    sd  $gp, 120($v0)

    # compute abs address of label finish
    la  $t9, finish
    # save address of finish as return-address for context-function
    # will be entered after context-function returns
    sd  $t9, 128($v0)

    jr  $ra # return pointer to context-data

finish:
    # reload our gp register (needed for la)
    addiu $t0, $sp, -144
    ld $gp, 120($t0)

    # call _exit(0)
    la $t9, _exit
    move $a0, $zero
    jr $t9
.end corocc_asm_make
.size corocc_asm_make, .-corocc_asm_make

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits

/* --- jump_fcontext (renamed corocc_asm_jump) --- */
/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |   0    |   8    |   16   |   24   |   32   | 40  |  *
 *  -------------------------------------------------  *
 *  |   F20  |   F22  |   F24  |   F26  |   F28  | F30 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |   48   |   56   |   64   |   72   |  80   | 88  |  *
 *  -------------------------------------------------  *
 *  |   S0   |   S1   |   S2   |   S3   |   S4   | S5  |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |   96   |  104   |  112   |  120   |  128  | 136 |  *
 *  -------------------------------------------------  *
 *  |   S6   |   S7   |   FP   |   GP   |   RA  | PC  |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "jump_mips32_n32_elf_gas.S"
.text
.globl corocc_asm_jump
.align 2
.type corocc_asm_jump,@function
.ent corocc_asm_jump
corocc_asm_jump:
    # reserve space on stack
    addiu $sp, $sp, -144

    sd  $s0, 48($sp)  # save S0
    sd  $s1, 56($sp)  # save S1
    sd  $s2, 64($sp)  # save S2
    sd  $s3, 72($sp)  # save S3
    sd  $s4, 80($sp)  # save S4
    sd  $s5, 88($sp)  # save S5
    sd  $s6, 96($sp)  # save S6
    sd  $s7, 104($sp) # save S7
    sd  $fp, 112($sp) # save FP
    sd  $ra, 128($sp) # save RA
    sd  $ra, 136($sp) # save RA as PC

#if defined(__mips_hard_float)
    s.d  $f20, ($sp)  # save F20
    s.d  $f22, 8($sp)  # save F22
    s.d  $f24, 16($sp)  # save F24
    s.d  $f26, 24($sp)  # save F26
    s.d  $f28, 32($sp)  # save F28
    s.d  $f30, 40($sp)  # save F30
#endif

    # store SP (pointing to old context-data) in V0 as return
    move  $v0, $sp

    # get SP (pointing to new context-data) from A0 param
    move  $sp, $a0

#if defined(__mips_hard_float)
    l.d  $f20, ($sp)  # restore F20
    l.d  $f22, 8($sp)  # restore F22
    l.d  $f24, 16($sp)  # restore F24
    l.d  $f26, 24($sp)  # restore F26
    l.d  $f28, 32($sp)  # restore F28
    l.d  $f30, 40($sp)  # restore F30
#endif

    ld  $s0, 48($sp)  # restore S0
    ld  $s1, 56($sp)  # restore S1
    ld  $s2, 64($sp)  # restore S2
    ld  $s3, 72($sp)  # restore S3
    ld  $s4, 80($sp)  # restore S4
    ld  $s5, 88($sp)  # restore S5
    ld  $s6, 96($sp)  # restore S6
    ld  $s7, 104($sp) # restore S7
    ld  $fp, 112($sp) # restore FP
    ld  $ra, 128($sp) # restore RA

    # load PC
    ld  $t9, 136($sp)

    # adjust stack
    addiu $sp, $sp, 144

    # N32 ABI: an 8-byte struct (coro_transfer) travels in ONE 64-bit
    # register, but the in-register layout is endianness dependent:
    #   big endian:    bits 63..32 = fctx, bits 31..0 = data
    #   little endian: bits 63..32 = data, bits 31..0 = fctx
    # (calibrated with -O0 -S probes for both -mabi=n32 toolchains; offset 0
    # of the struct occupies the most significant half on big endian.)
    # v0 currently holds the old sp (new fctx); a1 holds data.
#if defined(__MIPSEL__)
    dext   $v0, $v0, 0, 32  # fctx -> bits 31..0
    dsll32 $t0, $a1, 0      # data -> bits 63..32
#else
    dsll32 $v0, $v0, 0      # fctx -> bits 63..32 (low half cleared)
    dext   $t0, $a1, 0, 32  # data, zero-extended -> bits 31..0
#endif
    or     $v0, $v0, $t0    # packed coro_transfer as the return value
    move   $a0, $v0         # and as the first arg of the entry function

    # jump to context
    jr  $t9
.end corocc_asm_jump
.size corocc_asm_jump, .-corocc_asm_jump

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits
