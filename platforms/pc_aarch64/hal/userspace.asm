.section .text

.global aarch64_userspace_enter
aarch64_userspace_enter:
	msr elr_el1, x19
	msr sp_el0, x21
	msr spsr_el1, x22
	mov x0, x20
	isb
	eret

.section .note.GNU-stack,"",%progbits
