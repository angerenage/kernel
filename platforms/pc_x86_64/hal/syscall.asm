.intel_syntax noprefix

.section .text
.global x86_64_syscall_entry
.type x86_64_syscall_entry, @function
.extern syscall_dispatch

x86_64_syscall_entry:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15

	mov r12, rcx
	mov r13, r11
	mov r11, r9

	mov r9, r8
	mov r8, r10
	mov rcx, rdx
	mov rdx, rsi
	mov rsi, rdi
	mov rdi, rax

	sub rsp, 16
	mov qword ptr [rsp], r11
	call syscall_dispatch
	add rsp, 16

	mov rcx, r12
	mov r11, r13

	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx

	# Current callers enter from kernel mode; usermode sysret/iret return handling
	# belongs with the future userspace transition code.
	push r11
	popfq
	jmp rcx

.size x86_64_syscall_entry, . - x86_64_syscall_entry

.section .note.GNU-stack,"",@progbits
