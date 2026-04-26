.intel_syntax noprefix

.section .text
.global x86_64_userspace_enter
.type x86_64_userspace_enter, @function

x86_64_userspace_enter:
	cli
	mov rdi, r12
	iretq

.size x86_64_userspace_enter, . - x86_64_userspace_enter

.section .note.GNU-stack,"",@progbits
