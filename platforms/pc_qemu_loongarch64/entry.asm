.section .text
.global _start
.extern kernel_main

_start:
	la.local $sp, stack_top
	bl kernel_main

1:
	b 1b

.section .bss
.balign 16
stack_bottom:
	.space 0x4000
stack_top:

.section .note.GNU-stack,"",@progbits
