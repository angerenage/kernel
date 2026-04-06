.section .text
.global _start
.global stack_bottom
.global stack_top
.extern kernel_main

_start:
	la.local $sp, stack_top

	# LoongArch kernels here are built with the hard-float ABI, so FP must be
	# enabled before entering C.
	li.w $t0, 1
	csrwr $t0, 0x2

	bl kernel_main

1:
	b 1b

.section .bss
.balign 16
stack_bottom:
	.space 0x4000
stack_top:

.section .note.GNU-stack,"",@progbits
