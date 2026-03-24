.section .text
.global _start
.extern kernel_main

_start:
	la sp, stack_top
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
