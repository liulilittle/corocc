/*
 * Vendored from Boost.Context 1.92.0 (tag boost-1.92.0, commit
 * cc1b9e02cd530d6607becc984e1e5bfa4f7b3600)
 *   src/asm/make_sparc64_sysv_elf_gas.S
 *   src/asm/jump_sparc64_sysv_elf_gas.S
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

/* --- begin make_sparc64_sysv_elf_gas.S (renamed corocc_asm_make) --- */
/*
       Copyright Claudio Jeker 2024
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*
 * fcontext_t *make_fcontext(void *sp, size_t size, void (*fn)(transfer_t));
 */
#define CC64FSZ 176
#define BIAS 2047
#define FP 112
#define SP 128
#define I7 136

.file "make_sparc64_sysv_elf_gas.S"
.text
.align  4
.global corocc_asm_make
.type   corocc_asm_make, %function
corocc_asm_make:
	save	%sp, -CC64FSZ, %sp

	# shift address in %i0 (allocated stack) to lower 16 byte boundary
	and	%i0, -0xf, %i0

	# reserve space for two frames on the stack
	# the first frame is for the call the second one holds the data
	# for corocc_asm_jump
	sub	%i0, 2 * CC64FSZ, %i0

	# third argument of corocc_asm_make() is the context-function to call
	# store it in the first stack frame, also clear %fp there to indicate
	# the end of the stack.
	stx	%i2, [%i0 + CC64FSZ + I7]
	stx	%g0, [%i0 + CC64FSZ + FP]

	# On OpenBSD stackghost prevents overriding the return address on
	# a stack frame. So this code uses an extra trampoline to load
	# to call the context-function and then do the _exit(0) dance.
	# Extract the full address of the trampoline via pc relative addressing
1:
	rd	%pc, %l0
	add	%l0, (trampoline - 1b - 8), %l0
	stx	%l0, [%i0 + I7]

	# Save framepointer to first stack frame but first subtract the BIAS
	add	%i0, CC64FSZ - BIAS, %l0
	stx	%l0, [%i0 + SP]

	# Return context-data which is also includes the BIAS
	ret
	 restore %i0, -BIAS, %o0

trampoline:
	ldx	[%sp + BIAS + I7], %l0

	# no need to setup transfer_t, already in %o0 and %o1
	jmpl	%l0, %o7
	 nop

	call	_exit
	 clr	%o0
	unimp
.size	corocc_asm_make,.-corocc_asm_make
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits

/* --- begin jump_sparc64_sysv_elf_gas.S (renamed corocc_asm_jump) --- */
/*
       Copyright Claudio Jeker 2024
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*
 * typedef void*	fcontext_t;
 *
 * struct transfer_t {
 *	fcontext_t	fctx;
 *	void	*	data;
 * };
 *
 * transfer_t jump_fcontext(fcontext_t const to, void *vp);
 */
#define CC64FSZ 176
#define BIAS 2047
#define SP 128
#define I7 136

.file "jump_sparc64_sysv_elf_gas.S"
.text
.align  4
.global corocc_asm_jump
.type   corocc_asm_jump, %function
corocc_asm_jump:
	# prepare stack
	save	%sp, -CC64FSZ, %sp

	# store framepointer and return address in slots reserved
	# for arguments
	stx %fp, [%sp + BIAS + SP]
	stx %i7, [%sp + BIAS + I7]
	mov %sp, %o0
	# force flush register windows to stack and with that save context
	flushw
	# get SP (pointing to new context-data) from %i0 param
	mov %i0, %sp
	# load framepointer and return address from context
	ldx [%sp + BIAS + SP], %fp
	ldx [%sp + BIAS + I7], %i7

	ret
	 restore %o0, %g0, %o0
	# restore old %sp (pointing to old context-data) in %o0
	# *data stored in %o1 was not modified
.size	corocc_asm_jump,.-corocc_asm_jump
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
