/*
 * Vendored from Boost.Context 1.83.0
 *   src/asm/make_mips64_n64_elf_gas.S
 *   src/asm/jump_mips64_n64_elf_gas.S
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

/* --- begin make_mips64_n64_elf_gas.S (renamed corocc_asm_make) --- */
/*
            Copyright Jiaxun Yang 2018.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |     24    |  *
 *  -------------------------------------------------  *
 *  |    F24    |    F25    |    F26    |    F27    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |    F28    |    F29    |    F30    |    F31    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |     64    |    72     |     80    |     88    |  *
 *  -------------------------------------------------  *
 *  |    S0     |    S1     |     S2    |     S3    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 |  29 |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |  96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 |  *
 *  -------------------------------------------------  *
 *  |    S4     |    S5     |     S6    |     S7    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 |  *
 *  -------------------------------------------------  *
 *  |    FP     |    GP     |     RA    |     PC    |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "make_mips64_n64_elf_gas.S"
.text
.globl corocc_asm_make
.align 3
.type corocc_asm_make,@function
.ent corocc_asm_make
corocc_asm_make:
#ifdef __PIC__
.set    noreorder
.cpload $t9
.set    reorder
#endif
    # shift address in A0 to lower 16 byte boundary
    li $v1, 0xfffffffffffffff0
    and $v0, $v1, $a0

    # reserve space for context-data on context-stack
    daddiu $v0, $v0, -160

    # third arg of corocc_asm_make() == address of context-function
    sd  $a2, 152($v0)
    # save global pointer in context-data
    sd  $gp, 136($v0)

    # psudo instruction compute abs address of label finish based on GP
    dla  $t9, finish

    # save address of finish as return-address for context-function
    # will be entered after context-function returns
    sd  $t9, 144($v0)

    jr  $ra # return pointer to context-data

finish:
    # reload our gp register (needed for la)
    daddiu $t0, $sp, -160
    ld $gp, 136($t0)

    # call _exit(0)
    #  the previous function should have left the 16 bytes incoming argument
    #  area on the stack which we reuse for calling _exit
    dla $t9, _exit
    move $a0, $zero
    jr $t9
.end corocc_asm_make
.size corocc_asm_make, .-corocc_asm_make

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits

/* --- begin jump_mips64_n64_elf_gas.S (renamed corocc_asm_jump) --- */
/*
            Copyright Jiaxun Yang 2018.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |     24    |  *
 *  -------------------------------------------------  *
 *  |    F24    |    F25    |    F26    |    F27    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |    F28    |    F29    |    F30    |    F31    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |     64    |    72     |     80    |     88    |  *
 *  -------------------------------------------------  *
 *  |    S0     |    S1     |     S2    |     S3    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 |  29 |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |  96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 |  *
 *  -------------------------------------------------  *
 *  |    S4     |    S5     |     S6    |     S7    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 |  *
 *  -------------------------------------------------  *
 *  |    FP     |    GP     |     RA    |     PC    |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "jump_mips64_n64_elf_gas.S"
.text
.globl corocc_asm_jump
.align 3
.type corocc_asm_jump,@function
.ent corocc_asm_jump
corocc_asm_jump:
    # reserve space on stack
    daddiu $sp, $sp, -160

    sd  $s0, 64($sp)  # save S0
    sd  $s1, 72($sp)  # save S1
    sd  $s2, 80($sp)  # save S2
    sd  $s3, 88($sp)  # save S3
    sd  $s4, 96($sp)  # save S4
    sd  $s5, 104($sp) # save S5
    sd  $s6, 112($sp) # save S6
    sd  $s7, 120($sp) # save S7
    sd  $fp, 128($sp) # save FP
    sd  $ra, 144($sp) # save RA
    sd  $ra, 152($sp) # save RA as PC

#if defined(__mips_hard_float)
    s.d  $f24, 0($sp)   # save F24
    s.d  $f25, 8($sp)   # save F25
    s.d  $f26, 16($sp)  # save F26
    s.d  $f27, 24($sp)  # save F27
    s.d  $f28, 32($sp)  # save F28
    s.d  $f29, 40($sp)  # save F29
    s.d  $f30, 48($sp)  # save F30
    s.d  $f31, 56($sp)  # save F31
#endif

    # store SP (pointing to old context-data) in v0 as return
    move  $v0, $sp

    # get SP (pointing to new context-data) from a0 param
    move  $sp, $a0

#if defined(__mips_hard_float)
    l.d  $f24, 0($sp)   # restore F24
    l.d  $f25, 8($sp)   # restore F25
    l.d  $f26, 16($sp)  # restore F26
    l.d  $f27, 24($sp)  # restore F27
    l.d  $f28, 32($sp)  # restore F28
    l.d  $f29, 40($sp)  # restore F29
    l.d  $f30, 48($sp)  # restore F30
    l.d  $f31, 56($sp)  # restore F31
#endif

    ld  $s0, 64($sp)  # restore S0
    ld  $s1, 72($sp)  # restore S1
    ld  $s2, 80($sp)  # restore S2
    ld  $s3, 88($sp)  # restore S3
    ld  $s4, 96($sp)  # restore S4
    ld  $s5, 104($sp) # restore S5
    ld  $s6, 112($sp) # restore S6
    ld  $s7, 120($sp) # restore S7
    ld  $fp, 128($sp) # restore FP
    ld  $ra, 144($sp) # restore RAa

    # load PC
    ld  $t9, 152($sp)

    # adjust stack
    daddiu $sp, $sp, 160

    move  $a0, $v0 # move old sp from v0 to a0 as param
    move  $v1, $a1 # move *data from a1 to v1 as return

    # jump to context
    jr  $t9
.end corocc_asm_jump
.size corocc_asm_jump, .-corocc_asm_jump

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits
