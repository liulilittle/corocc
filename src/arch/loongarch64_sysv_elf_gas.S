/*
 * Vendored from Boost.Context 1.92.0 (tag boost-1.92.0, commit
 * cc1b9e02cd530d6607becc984e1e5bfa4f7b3600)
 *   src/asm/make_loongarch64_sysv_elf_gas.S
 *   src/asm/jump_loongarch64_sysv_elf_gas.S
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

/* --- begin make_loongarch64_sysv_elf_gas.S (renamed corocc_asm_make) --- */
/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |     24    |  *
 *  -------------------------------------------------  *
 *  |    FS0    |    FS1    |    FS2    |    FS3    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |    FS4    |    FS5    |    FS6    |    FS7    |  *
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
 *  |    S8     |    FP     |     RA    |     PC    |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "make_loongarch64_sysv_elf_gas.S"
.text
.globl corocc_asm_make
.hidden corocc_asm_make
.align 2
.type corocc_asm_make,@function
corocc_asm_make:
    # shift address in A0 to lower 16 byte boundary
    bstrins.d $a0, $zero, 3, 0

    # reserve space for context-data on context-stack
    addi.d  $a0, $a0, -160

    # third arg of corocc_asm_make() == address of context-function
    st.d  $a2, $a0, 152

    # save address of finish as return-address for context-function
    # will be entered after context-function returns
    la.local  $a4, finish
    st.d  $a4, $a0, 144

    # return pointer to context-data
    jr  $ra

finish:
    # exit code is zero
    li.d  $a0, 0
    # call _exit(0)
    b  %plt(_exit)

.size corocc_asm_make, .-corocc_asm_make
/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits

/* --- begin jump_loongarch64_sysv_elf_gas.S (renamed corocc_asm_jump) --- */
/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |     24    |  *
 *  -------------------------------------------------  *
 *  |    FS0    |    FS1    |    FS2    |    FS3    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |    FS4    |    FS5    |    FS6    |    FS7    |  *
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
 *  |    S8     |    FP     |     RA    |     PC    |  *
 *  -------------------------------------------------  *
 *                                                     *
 * *****************************************************/

.file "jump_loongarch64_sysv_elf_gas.S"
.text
.globl corocc_asm_jump
.hidden corocc_asm_jump
.align 2
.type corocc_asm_jump,@function
corocc_asm_jump:
    # reserve space on stack
    addi.d  $sp, $sp, -160

    # save fs0 - fs7
    fst.d  $fs0, $sp, 0
    fst.d  $fs1, $sp, 8
    fst.d  $fs2, $sp, 16
    fst.d  $fs3, $sp, 24
    fst.d  $fs4, $sp, 32
    fst.d  $fs5, $sp, 40
    fst.d  $fs6, $sp, 48
    fst.d  $fs7, $sp, 56

    # save s0 - s8, fp, ra
    st.d  $s0, $sp, 64
    st.d  $s1, $sp, 72
    st.d  $s2, $sp, 80
    st.d  $s3, $sp, 88
    st.d  $s4, $sp, 96
    st.d  $s5, $sp, 104
    st.d  $s6, $sp, 112
    st.d  $s7, $sp, 120
    st.d  $s8, $sp, 128
    st.d  $fp, $sp, 136
    st.d  $ra, $sp, 144

    # save RA as PC
    st.d  $ra, $sp, 152

    # store SP (pointing to context-data) in A2
    move  $a2, $sp

    # restore SP (pointing to context-data) from A0
    move  $sp, $a0

    # load fs0 - fs7
    fld.d  $fs0, $sp, 0
    fld.d  $fs1, $sp, 8
    fld.d  $fs2, $sp, 16
    fld.d  $fs3, $sp, 24
    fld.d  $fs4, $sp, 32
    fld.d  $fs5, $sp, 40
    fld.d  $fs6, $sp, 48
    fld.d  $fs7, $sp, 56

    #load s0 - s7
    ld.d  $s0, $sp, 64
    ld.d  $s1, $sp, 72
    ld.d  $s2, $sp, 80
    ld.d  $s3, $sp, 88
    ld.d  $s4, $sp, 96
    ld.d  $s5, $sp, 104
    ld.d  $s6, $sp, 112
    ld.d  $s7, $sp, 120
    ld.d  $s8, $sp, 128
    ld.d  $fp, $sp, 136
    ld.d  $ra, $sp, 144

    # return transfer_t from jump
    # pass transfer_t as first arg in context function
    # a0 == FCTX, a1 == DATA
    move  $a0, $a2

    # load PC
    ld.d  $a2, $sp, 152

    # restore stack
    addi.d  $sp, $sp, 160

    # jump to context
    jr  $a2
.size corocc_asm_jump, .-corocc_asm_jump

/* Mark that we don't need executable stack.  */
.section .note.GNU-stack,"",%progbits
