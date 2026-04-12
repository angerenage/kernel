.intel_syntax noprefix

.section .text

.equ THREAD_CTX_IP, 0
.equ THREAD_CTX_SP, 8
.equ THREAD_CTX_SPILL, 16
.equ THREAD_CTX_RBX, THREAD_CTX_SPILL + 0
.equ THREAD_CTX_RBP, THREAD_CTX_SPILL + 8
.equ THREAD_CTX_R12, THREAD_CTX_SPILL + 16
.equ THREAD_CTX_R13, THREAD_CTX_SPILL + 24
.equ THREAD_CTX_R14, THREAD_CTX_SPILL + 32
.equ THREAD_CTX_R15, THREAD_CTX_SPILL + 40

.global x86_64_thread_context_switch
x86_64_thread_context_switch:
	mov [rdi + THREAD_CTX_RBX], rbx
	mov [rdi + THREAD_CTX_RBP], rbp
	mov [rdi + THREAD_CTX_R12], r12
	mov [rdi + THREAD_CTX_R13], r13
	mov [rdi + THREAD_CTX_R14], r14
	mov [rdi + THREAD_CTX_R15], r15
	mov [rdi + THREAD_CTX_SP], rsp
	lea rax, [rip + .Lresume]
	mov [rdi + THREAD_CTX_IP], rax

	mov rbx, [rsi + THREAD_CTX_RBX]
	mov rbp, [rsi + THREAD_CTX_RBP]
	mov r12, [rsi + THREAD_CTX_R12]
	mov r13, [rsi + THREAD_CTX_R13]
	mov r14, [rsi + THREAD_CTX_R14]
	mov r15, [rsi + THREAD_CTX_R15]
	mov rsp, [rsi + THREAD_CTX_SP]
	mov rax, [rsi + THREAD_CTX_IP]
	jmp rax

.Lresume:
	ret

.global x86_64_thread_entry
x86_64_thread_entry:
	mov rdi, r13
	jmp r12

.section .note.GNU-stack,"",@progbits
