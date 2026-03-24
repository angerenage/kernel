.section .text
.global _start
.extern kernel_main

_start:
	adrp x0, stack_top
	add x0, x0, :lo12:stack_top
	mov sp, x0
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
