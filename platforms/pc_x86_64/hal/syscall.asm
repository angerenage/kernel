.intel_syntax noprefix

.section .text
.global x86_64_syscall_entry
.type x86_64_syscall_entry, @function
.extern x86_64_handle_interrupt
.extern x86_64_maybe_preempt_on_interrupt_exit
.extern x86_64_prepare_user_return
.extern x86_64_classify_user_return
.extern x86_64_reject_user_return
.extern x86_64_interrupt_restore

.equ CPU_KERNEL_ENTRY_STACK_TOP, 40
.equ CPU_SYSCALL_USER_STACK, 48

.equ X86_FRAME_RAX, 0
.equ X86_FRAME_RBX, 8
.equ X86_FRAME_RCX, 16
.equ X86_FRAME_RDX, 24
.equ X86_FRAME_RBP, 32
.equ X86_FRAME_RDI, 40
.equ X86_FRAME_RSI, 48
.equ X86_FRAME_R8, 56
.equ X86_FRAME_R9, 64
.equ X86_FRAME_R10, 72
.equ X86_FRAME_R11, 80
.equ X86_FRAME_R12, 88
.equ X86_FRAME_R13, 96
.equ X86_FRAME_R14, 104
.equ X86_FRAME_R15, 112
.equ X86_FRAME_VECTOR, 120
.equ X86_FRAME_ERROR, 128
.equ X86_FRAME_RIP, 136
.equ X86_FRAME_CS, 144
.equ X86_FRAME_RFLAGS, 152
.equ X86_FRAME_RSP, 160
.equ X86_FRAME_SS, 168
.equ X86_USER_FRAME_SIZE, 176

.equ X86_SYSCALL_VECTOR, 0x80
.equ X86_USER_CS, 0x2b
.equ X86_USER_SS, 0x23

.equ X86_USER_RETURN_SYSRET, 0
.equ X86_USER_RETURN_IRET, 1

x86_64_syscall_entry:
	/* Preserve the only user value not already held in a general register. */
	mov qword ptr gs:[CPU_SYSCALL_USER_STACK], rsp
	mov rsp, qword ptr gs:[CPU_KERNEL_ENTRY_STACK_TOP]
	and rsp, -16
	sub rsp, X86_USER_FRAME_SIZE

	/* Build the same register prefix used by x86_64_interrupt_common. */
	mov qword ptr [rsp + X86_FRAME_RAX], rax
	mov qword ptr [rsp + X86_FRAME_RBX], rbx
	mov qword ptr [rsp + X86_FRAME_RCX], rcx
	mov qword ptr [rsp + X86_FRAME_RDX], rdx
	mov qword ptr [rsp + X86_FRAME_RBP], rbp
	mov qword ptr [rsp + X86_FRAME_RDI], rdi
	mov qword ptr [rsp + X86_FRAME_RSI], rsi
	mov qword ptr [rsp + X86_FRAME_R8], r8
	mov qword ptr [rsp + X86_FRAME_R9], r9
	mov qword ptr [rsp + X86_FRAME_R10], r10
	mov qword ptr [rsp + X86_FRAME_R11], r11
	mov qword ptr [rsp + X86_FRAME_R12], r12
	mov qword ptr [rsp + X86_FRAME_R13], r13
	mov qword ptr [rsp + X86_FRAME_R14], r14
	mov qword ptr [rsp + X86_FRAME_R15], r15

	mov rax, X86_SYSCALL_VECTOR
	mov qword ptr [rsp + X86_FRAME_VECTOR], rax
	xor eax, eax
	mov qword ptr [rsp + X86_FRAME_ERROR], rax
	mov qword ptr [rsp + X86_FRAME_RIP], rcx
	mov rax, X86_USER_CS
	mov qword ptr [rsp + X86_FRAME_CS], rax
	mov qword ptr [rsp + X86_FRAME_RFLAGS], r11
	mov rax, qword ptr gs:[CPU_SYSCALL_USER_STACK]
	mov qword ptr [rsp + X86_FRAME_RSP], rax
	mov rax, X86_USER_SS
	mov qword ptr [rsp + X86_FRAME_SS], rax

	mov rdi, rsp
	call x86_64_handle_interrupt
	call x86_64_maybe_preempt_on_interrupt_exit
	call x86_64_prepare_user_return

	mov rdi, rsp
	call x86_64_classify_user_return
	cmp eax, X86_USER_RETURN_SYSRET
	je .Lx86_64_sysret_restore
	cmp eax, X86_USER_RETURN_IRET
	je x86_64_interrupt_restore
	call x86_64_reject_user_return

.Lx86_64_sysret_restore:
	/* RCX and R11 are architectural SYSCALL clobbers and carry SYSRET state. */
	mov rax, qword ptr [rsp + X86_FRAME_RAX]
	mov rbx, qword ptr [rsp + X86_FRAME_RBX]
	mov rdx, qword ptr [rsp + X86_FRAME_RDX]
	mov rbp, qword ptr [rsp + X86_FRAME_RBP]
	mov rdi, qword ptr [rsp + X86_FRAME_RDI]
	mov rsi, qword ptr [rsp + X86_FRAME_RSI]
	mov r8, qword ptr [rsp + X86_FRAME_R8]
	mov r9, qword ptr [rsp + X86_FRAME_R9]
	mov r10, qword ptr [rsp + X86_FRAME_R10]
	mov r12, qword ptr [rsp + X86_FRAME_R12]
	mov r13, qword ptr [rsp + X86_FRAME_R13]
	mov r14, qword ptr [rsp + X86_FRAME_R14]
	mov r15, qword ptr [rsp + X86_FRAME_R15]
	mov rcx, qword ptr [rsp + X86_FRAME_RIP]
	mov r11, qword ptr [rsp + X86_FRAME_RFLAGS]
	mov rsp, qword ptr [rsp + X86_FRAME_RSP]
	sysretq

.size x86_64_syscall_entry, . - x86_64_syscall_entry

.section .note.GNU-stack,"",@progbits
