/*
 * riscv32 context-switch backend (RV32GC, ILP32D), derived from
 * riscv64_sysv_elf_gas.S in this directory (Boost.Context 1.83.0,
 * Boost Software License 1.0; see THIRD-PARTY.md) by adapting the
 * register frame from LP64 to ILP32D:
 *   - integer saves use sw/lw (4-byte slots)
 *   - floating-point saves stay fsd/fld (the D ABI keeps 64-bit f-regs)
 *   - frame is 0xa0 so the restored stack stays 16-byte aligned
 * Public symbols follow the repository convention:
 *   corocc_asm_make / corocc_asm_jump
 */

/* --- make_fcontext (renamed corocc_asm_make) --- */
/*******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  | 0x00 | 0x08 | 0x10 | 0x18 | 0x20 | 0x28 | 0x30 | 0x38 |  *
 *  -------------------------------------------------  *
 *  |  fs0  |  fs1  |  fs2  |  fs3  |  fs4  |  fs5  |  fs6  |  fs7  |  *
 *  -------------------------------------------------  *
 *  | 0x40 | 0x48 | 0x50 | 0x58 | 0x60 | 0x64 | 0x68 | 0x6c |  *
 *  -------------------------------------------------  *
 *  |  fs8  |  fs9  | fs10  | fs11  |  s0   |  s1   |  s2   |  s3   |  *
 *  -------------------------------------------------  *
 *  | 0x70 | 0x74 | 0x78 | 0x7c | 0x80 | 0x84 | 0x88 | 0x8c |  *
 *  -------------------------------------------------  *
 *  |  s4   |  s5   |  s6   |  s7   |  s8   |  s9   | s10   | s11   |  *
 *  -------------------------------------------------  *
 *  | 0x90 | 0x94 | 0x98 | 0x9c |                               *
 *  -------------------------------------------------          *
 *  |  ra   |  pc   |           padding                        |  *
 *  -------------------------------------------------          *
 *                                                     *
 *******************************************************/

.file "riscv32_sysv_elf_gas.S"
.text
.align  1
.global corocc_asm_make
.type   corocc_asm_make, %function
corocc_asm_make:
    # shift address in a0 (allocated stack) to lower 16 byte boundary
    andi a0, a0, ~0xF

    # reserve space for context-data on context-stack
    addi  a0, a0, -0xa0

    # third arg of corocc_asm_make() == address of context-function
    # store address as a PC to jump in
    sw  a2, 0x94(a0)

    # save address of finish as return-address for context-function
    # will be entered after context-function returns (RA register)
    lla  a4, finish
    sw  a4, 0x90(a0)

    ret // return pointer to context-data (a0)

finish:
    # exit code is zero
    li  a0, 0
    # exit application
    tail  _exit@plt

.size   corocc_asm_make,.-corocc_asm_make
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits

/* --- jump_fcontext (renamed corocc_asm_jump) --- */
.text
.align  1
.global corocc_asm_jump
.type   corocc_asm_jump, %function
corocc_asm_jump:
    # prepare stack for GP + FPU
    addi  sp, sp, -0xa0

    # save fs0 - fs11
    fsd  fs0, 0x00(sp)
    fsd  fs1, 0x08(sp)
    fsd  fs2, 0x10(sp)
    fsd  fs3, 0x18(sp)
    fsd  fs4, 0x20(sp)
    fsd  fs5, 0x28(sp)
    fsd  fs6, 0x30(sp)
    fsd  fs7, 0x38(sp)
    fsd  fs8, 0x40(sp)
    fsd  fs9, 0x48(sp)
    fsd  fs10, 0x50(sp)
    fsd  fs11, 0x58(sp)

    # save s0-s11, ra
    sw  s0, 0x60(sp)
    sw  s1, 0x64(sp)
    sw  s2, 0x68(sp)
    sw  s3, 0x6c(sp)
    sw  s4, 0x70(sp)
    sw  s5, 0x74(sp)
    sw  s6, 0x78(sp)
    sw  s7, 0x7c(sp)
    sw  s8, 0x80(sp)
    sw  s9, 0x84(sp)
    sw  s10, 0x88(sp)
    sw  s11, 0x8c(sp)
    sw  ra, 0x90(sp)

    # save RA as PC
    sw  ra, 0x94(sp)

    # store SP (pointing to context-data) in A2
    mv  a2, sp

    # restore SP (pointing to context-data) from A0
    mv  sp, a0

    # load fs0 - fs11
    fld  fs0, 0x00(sp)
    fld  fs1, 0x08(sp)
    fld  fs2, 0x10(sp)
    fld  fs3, 0x18(sp)
    fld  fs4, 0x20(sp)
    fld  fs5, 0x28(sp)
    fld  fs6, 0x30(sp)
    fld  fs7, 0x38(sp)
    fld  fs8, 0x40(sp)
    fld  fs9, 0x48(sp)
    fld  fs10, 0x50(sp)
    fld  fs11, 0x58(sp)

    # load s0-s11, ra
    lw  s0, 0x60(sp)
    lw  s1, 0x64(sp)
    lw  s2, 0x68(sp)
    lw  s3, 0x6c(sp)
    lw  s4, 0x70(sp)
    lw  s5, 0x74(sp)
    lw  s6, 0x78(sp)
    lw  s7, 0x7c(sp)
    lw  s8, 0x80(sp)
    lw  s9, 0x84(sp)
    lw  s10, 0x88(sp)
    lw  s11, 0x8c(sp)
    lw  ra, 0x90(sp)

    # return transfer_t from jump
    # pass transfer_t as first arg in context function
    # a0 == FCTX, a1 == DATA
    mv  a0, a2

    # load pc
    lw  a2, 0x94(sp)

    # restore stack from GP + FPU
    addi  sp, sp, 0xa0

    jr  a2
.size   corocc_asm_jump,.-corocc_asm_jump
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
