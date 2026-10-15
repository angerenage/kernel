.intel_syntax noprefix

.section .text

.global libc_setjmp
libc_setjmp:
	mov [rdi + 0], rbx
	mov [rdi + 8], rbp
	mov [rdi + 16], r12
	mov [rdi + 24], r13
	mov [rdi + 32], r14
	mov [rdi + 40], r15
	lea rax, [rsp + 8]
	mov [rdi + 48], rax
	mov rax, [rsp]
	mov [rdi + 56], rax
	xor eax, eax
	ret

.global longjmp
longjmp:
	mov rbx, [rdi + 0]
	mov rbp, [rdi + 8]
	mov r12, [rdi + 16]
	mov r13, [rdi + 24]
	mov r14, [rdi + 32]
	mov r15, [rdi + 40]
	mov rsp, [rdi + 48]
	mov eax, esi
	test eax, eax
	jne .Ljump
	mov eax, 1
.Ljump:
	jmp [rdi + 56]

.section .note.GNU-stack,"",@progbits
