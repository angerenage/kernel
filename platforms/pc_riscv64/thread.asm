.section .text

.equ THREAD_CTX_IP, 0
.equ THREAD_CTX_SP, 8
.equ THREAD_CTX_SPILL, 16
.equ THREAD_CTX_S0, THREAD_CTX_SPILL + 0
.equ THREAD_CTX_S1, THREAD_CTX_SPILL + 8
.equ THREAD_CTX_S2, THREAD_CTX_SPILL + 16
.equ THREAD_CTX_S3, THREAD_CTX_SPILL + 24
.equ THREAD_CTX_S4, THREAD_CTX_SPILL + 32
.equ THREAD_CTX_S5, THREAD_CTX_SPILL + 40
.equ THREAD_CTX_S6, THREAD_CTX_SPILL + 48
.equ THREAD_CTX_S7, THREAD_CTX_SPILL + 56
.equ THREAD_CTX_S8, THREAD_CTX_SPILL + 64
.equ THREAD_CTX_S9, THREAD_CTX_SPILL + 72
.equ THREAD_CTX_S10, THREAD_CTX_SPILL + 80
.equ THREAD_CTX_S11, THREAD_CTX_SPILL + 88

.global riscv64_thread_context_switch
riscv64_thread_context_switch:
	sd s0, THREAD_CTX_S0(a0)
	sd s1, THREAD_CTX_S1(a0)
	sd s2, THREAD_CTX_S2(a0)
	sd s3, THREAD_CTX_S3(a0)
	sd s4, THREAD_CTX_S4(a0)
	sd s5, THREAD_CTX_S5(a0)
	sd s6, THREAD_CTX_S6(a0)
	sd s7, THREAD_CTX_S7(a0)
	sd s8, THREAD_CTX_S8(a0)
	sd s9, THREAD_CTX_S9(a0)
	sd s10, THREAD_CTX_S10(a0)
	sd s11, THREAD_CTX_S11(a0)
	sd sp, THREAD_CTX_SP(a0)
	la t0, .Lresume
	sd t0, THREAD_CTX_IP(a0)

	ld s0, THREAD_CTX_S0(a1)
	ld s1, THREAD_CTX_S1(a1)
	ld s2, THREAD_CTX_S2(a1)
	ld s3, THREAD_CTX_S3(a1)
	ld s4, THREAD_CTX_S4(a1)
	ld s5, THREAD_CTX_S5(a1)
	ld s6, THREAD_CTX_S6(a1)
	ld s7, THREAD_CTX_S7(a1)
	ld s8, THREAD_CTX_S8(a1)
	ld s9, THREAD_CTX_S9(a1)
	ld s10, THREAD_CTX_S10(a1)
	ld s11, THREAD_CTX_S11(a1)
	ld sp, THREAD_CTX_SP(a1)
	ld t0, THREAD_CTX_IP(a1)
	jr t0

.Lresume:
	ret

.global riscv64_thread_entry
riscv64_thread_entry:
	mv a0, s1
	jr s0

.section .note.GNU-stack,"",@progbits
