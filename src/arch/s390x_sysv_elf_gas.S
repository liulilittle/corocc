/*
 * Vendored from Boost.Context 1.83.0
 *   src/asm/make_s390x_sysv_elf_gas.S
 *   src/asm/jump_s390x_sysv_elf_gas.S
 * Distributed under the Boost Software License, Version 1.0.
 * See THIRD-PARTY.md for origin, version and license.
 *
 * Only the public symbol names were renamed so that corocc can coexist in one
 * binary with Boost:
 *   make_fcontext -> corocc_asm_make
 *   jump_fcontext -> corocc_asm_jump
 * The machine code is unchanged; the original copyright headers are kept
 * verbatim.
 *
 * Cross-thread migration caveat (s390x): GCC materializes the TLS pointer
 * (from access registers a0/a1) into a callee-saved GPR and may reuse it
 * across calls within a function (e.g. the stack-protector canary check).
 * A context resumed on a different thread would then use a stale TLS
 * pointer. The s390x builds therefore compile the library and tests with
 * -fno-stack-protector (see scripts/toolchains/s390x.cmake); see the P11
 * report for the full analysis.
 */

/* --- begin make_s390x_sysv_elf_gas.S (renamed corocc_asm_make) --- */
/*******************************************************
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |    24     |  *
 *  -------------------------------------------------  *
 *  |   t.fctx  |   t.data  |    r2     |    r6     |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |     r7    |     r8    |     r9    |    r10    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |     64    |     72    |     80    |     88    |  *
 *  -------------------------------------------------  *
 *  |    r11    |    r12    |    r13    |    r14    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 | 29  |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |     96    |    104    |    112    |    120    |  *
 *  -------------------------------------------------  *
 *  |     f8    |     f9    |    f10    |    f11    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  |    128    |    136    |    144    |    152    |  *
 *  -------------------------------------------------  *
 *  |    f12    |    f13    |    f14    |    f15    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  40 |  41 |  42 |  43 |  44 |  45 |  46 |  47 |  *
 *  -------------------------------------------------  *
 *  |    160    |    168    |    176    |           |  *
 *  -------------------------------------------------  *
 *  |    fpc    |     pc    |           |           |  *
 *  -------------------------------------------------  *
 *******************************************************/

.text
.align	8
.global	corocc_asm_make
.type	corocc_asm_make, @function

#define ARG_OFFSET         0
#define GR_OFFSET          16
#define R14_OFFSET         88
#define FP_OFFSET          96
#define FPC_OFFSET         160
#define PC_OFFSET          168
#define CONTEXT_SIZE       176

/*

fcontext_t corocc_asm_make( void * sp, std::size_t size, void (* fn)( transfer_t) );

Create and return a context below SP to call FN.

Incoming args
r2 - The stack location where to create the context
r3 - The size of the context
r4 - The address of the context function

*/

corocc_asm_make:
	.machine "z10"
	/* Align the stack to an 8 byte boundary.  */
	nill    %r2,0xfff8

	/* Allocate stack space for the context.  */
	aghi	%r2,-CONTEXT_SIZE

	/* Set the r2 save slot to zero.  This indicates jump_fcontext
	   that this is a special context.  */
	mvghi	GR_OFFSET(%r2),0

	/* Save the floating point control register.  */
	stfpc	FPC_OFFSET(%r2)

	/* Store the address of the target function as new pc.  */
	stg	%r4,PC_OFFSET(%r2)

	/* Store a pointer to the finish routine as r14. If a function
	   called via context routines just returns that value will be
	   loaded and used as return address.  Hence the program will
	   just exit.  */
	larl	%r1,finish
	stg	%r1,R14_OFFSET(%r2)

	/* Return as usual with the new context returned in r2.  */
	br	%r14

finish:
	/* In finish tasks, you load the exit code and exit the
	   corocc_asm_make This is called when the context-function is
	   entirely executed.  */
	lghi	%r2,0
	brasl	%r14,_exit@PLT

.size   corocc_asm_make,.-corocc_asm_make
.section .note.GNU-stack,"",%progbits

/* --- begin jump_s390x_sysv_elf_gas.S (renamed corocc_asm_jump) --- */
/*******************************************************
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |     0     |     8     |    16     |    24     |  *
 *  -------------------------------------------------  *
 *  |   t.fctx  |   t.data  |    r2     |    r6     |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |     32    |    40     |     48    |     56    |  *
 *  -------------------------------------------------  *
 *  |     r7    |     r8    |     r9    |    r10    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |     64    |     72    |     80    |     88    |  *
 *  -------------------------------------------------  *
 *  |    r11    |    r12    |    r13    |    r14    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 | 29  |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |     96    |    104    |    112    |    120    |  *
 *  -------------------------------------------------  *
 *  |     f8    |     f9    |    f10    |    f11    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  |    128    |    136    |    144    |    152    |  *
 *  -------------------------------------------------  *
 *  |    f12    |    f13    |    f14    |    f15    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  40 |  41 |  42 |  43 |  44 |  45 |  46 |  47 |  *
 *  -------------------------------------------------  *
 *  |    160    |    168    |    176    |           |  *
 *  -------------------------------------------------  *
 *  |    fpc    |     pc    |           |           |  *
 *  -------------------------------------------------  *
 *******************************************************/

.text
.align	8
.global	corocc_asm_jump
.type	corocc_asm_jump, @function

#define ARG_OFFSET         0
#define GR_OFFSET          16
#define R14_OFFSET         88
#define FP_OFFSET          96
#define FPC_OFFSET         160
#define PC_OFFSET          168
#define CONTEXT_SIZE       176

#define REG_SAVE_AREA_SIZE 160

/*

typedef void*   fcontext_t;

struct transfer_t {
   fcontext_t  fctx;
   void    *   data;
};

transfer_t corocc_asm_jump( fcontext_t const to,
			  void * data);

Incoming args
r2 - Hidden argument to the location where the return transfer_t needs to be returned
r3 - Context we want to switch to
r4 - Data pointer

*/

corocc_asm_jump:
	.machine "z10"
	/* Reserve stack space to store the current context.  */
	aghi	%r15,-CONTEXT_SIZE

	/* Save the argument register holding the location of the return value.  */
	stg	%r2,GR_OFFSET(%r15)

	/* Save the call-saved general purpose registers.  */
	stmg	%r6,%r14,GR_OFFSET+8(%r15)

	/* Save call-saved floating point registers.  */
	std	%f8,FP_OFFSET(%r15)
	std	%f9,FP_OFFSET+8(%r15)
	std	%f10,FP_OFFSET+16(%r15)
	std	%f11,FP_OFFSET+24(%r15)
	std	%f12,FP_OFFSET+32(%r15)
	std	%f13,FP_OFFSET+40(%r15)
	std	%f14,FP_OFFSET+48(%r15)
	std	%f15,FP_OFFSET+56(%r15)

	/* Save the return address as current pc.  */
	stg	%r14,PC_OFFSET(%r15)

	/* Save the floating point control register.  */
	stfpc	FPC_OFFSET(%r15)

	/* Backup the stack pointer pointing to the old context-data into r1.  */
	lgr	 %r1,%r15

	/* Load the new context pointer as stack pointer.  */
	lgr	%r15,%r3

	/* Restore the call-saved GPRs from the new context.  */
	lmg	%r6,%r14,GR_OFFSET+8(%r15)

	/* Restore call-saved floating point registers.  */
	ld	%f8,FP_OFFSET(%r15)
	ld	%f9,FP_OFFSET+8(%r15)
	ld	%f10,FP_OFFSET+16(%r15)
	ld	%f11,FP_OFFSET+24(%r15)
	ld	%f12,FP_OFFSET+32(%r15)
	ld	%f13,FP_OFFSET+40(%r15)
	ld	%f14,FP_OFFSET+48(%r15)
	ld	%f15,FP_OFFSET+56(%r15)

	/* Load the floating point control register.  */
	lfpc	FPC_OFFSET(%r15)

	/* Restore PC - the location where we will jump to at the end.  */
	lg	%r5,PC_OFFSET(%r15)

	ltg	%r2,GR_OFFSET(%r15)
	jnz	use_return_slot

	/* We're restoring a context created by make_fcontext.
	   This is going to be the argument of the entry point
	   of the fiber. We're placing it on top of the ABI
	   defined register save area of the fiber's own stack. */
	la	%r2,REG_SAVE_AREA_SIZE(%r15)

	/* REG_SAVE_AREA_SIZE + sizeof(transfer_t) */
	aghi	%r15,-(REG_SAVE_AREA_SIZE+16)

use_return_slot:
	/* Save the two fields in transfer_t.  When calling a
	   make_fcontext function this becomes the function argument of
	   the target function, otherwise it will be the return value of
	   corocc_asm_jump.  */
	stg	%r1,0(%r2)
	stg	%r4,8(%r2)

	/* Free the restored context.  */
	aghi	%r15,CONTEXT_SIZE

	/* Jump to the PC loaded from the new context.  */
	br	%r5


.size   corocc_asm_jump,.-corocc_asm_jump
.section .note.GNU-stack,"",%progbits
