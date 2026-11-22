.section .text
.global _start
.global stack_bottom
.global stack_top
.extern kernel_main

_start:
	# Clear CRMD.IE before relying on any bootloader-provided state.
	csrrd $t0, 0x0
	bstrins.d $t0, $zero, 2, 2
	csrwr $t0, 0x0

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
