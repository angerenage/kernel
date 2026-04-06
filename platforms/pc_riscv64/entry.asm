.section .text
.global _start
.global stack_bottom
.global stack_top
.extern kernel_main

_start:
	la sp, stack_top
	csrci sstatus, 2
	csrw sie, zero
	call kernel_main

1:
	wfi
	j 1b

.section .bss
.balign 16
stack_bottom:
	.skip 0x4000
stack_top:

.section .note.GNU-stack,"",@progbits
