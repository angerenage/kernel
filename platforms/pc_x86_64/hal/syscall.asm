.intel_syntax noprefix

.section .text
.global x86_64_syscall_entry
.type x86_64_syscall_entry, @function
.extern syscall_dispatch

.equ CPU_KERNEL_ENTRY_STACK_TOP, 40
.equ CPU_SYSCALL_USER_STACK, 48

x86_64_syscall_entry:
	mov qword ptr gs:[CPU_SYSCALL_USER_STACK], rsp
	mov rsp, qword ptr gs:[CPU_KERNEL_ENTRY_STACK_TOP]
	and rsp, -16

	push qword ptr gs:[CPU_SYSCALL_USER_STACK]
	push rcx
	push r11
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15

	mov r11, r9

	mov r9, r8
	mov r8, r10
	mov rcx, rdx
	mov rdx, rsi
	mov rsi, rdi
	mov rdi, rax

	push r11
	call syscall_dispatch
	add rsp, 8

	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	pop r11
	pop rcx
	pop r10

	mov rsp, r10
	sysretq

.size x86_64_syscall_entry, . - x86_64_syscall_entry

.section .note.GNU-stack,"",@progbits
