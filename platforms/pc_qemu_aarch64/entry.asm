.section .text
.global _start
.extern kernel_main

_start:
	adrp x0, stack_top
	add x0, x0, :lo12:stack_top
	msr spsel, #1
	mov sp, x0
	msr daifset, #0xf

	// Enable FP/SIMD at EL1 before entering C so variadic code can spill q-regs.
	mrs x0, cpacr_el1
	orr x0, x0, #(3 << 20)
	msr cpacr_el1, x0
	isb
	msr fpcr, xzr
	msr fpsr, xzr

	bl kernel_main

1:
	wfe
	b 1b

.section .bss
.balign 16
stack_bottom:
	.skip 0x4000
stack_top:

.section .note.GNU-stack,"",%progbits
