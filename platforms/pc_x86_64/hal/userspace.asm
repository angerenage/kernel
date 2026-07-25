.intel_syntax noprefix

.section .text
.global x86_64_userspace_enter
.type x86_64_userspace_enter, @function
.extern x86_64_prepare_user_return
.extern sched_complete_context_switch

x86_64_userspace_enter:
	cli
	sub rsp, 8
	call sched_complete_context_switch
	call x86_64_prepare_user_return
	add rsp, 8

	mov rdi, r12
	iretq

.size x86_64_userspace_enter, . - x86_64_userspace_enter

.section .note.GNU-stack,"",@progbits
