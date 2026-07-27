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


.global x86_64_fp_init
x86_64_fp_init:
	mov rax, cr0
	and rax, -13
	or rax, 0x22
	mov cr0, rax
	mov rax, cr4
	or rax, (1 << 9) | (1 << 10)
	mov cr4, rax
	fninit
	ldmxcsr [rip + .Lx86_default_mxcsr]
	mov eax, 1
	ret

.global x86_64_fp_context_save
x86_64_fp_context_save:
	fxsave64 [rdi]
	ret

.global x86_64_fp_context_restore
x86_64_fp_context_restore:
	fxrstor64 [rdi]
	ret

.section .rodata
.balign 4
.Lx86_default_mxcsr:
	.long 0x1f80

.section .note.GNU-stack,"",@progbits
