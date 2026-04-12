.section .text

.equ THREAD_CTX_IP, 0
.equ THREAD_CTX_SP, 8
.equ THREAD_CTX_SPILL, 16
.equ THREAD_CTX_FP, THREAD_CTX_SPILL + 0
.equ THREAD_CTX_S0, THREAD_CTX_SPILL + 8
.equ THREAD_CTX_S1, THREAD_CTX_SPILL + 16
.equ THREAD_CTX_S2, THREAD_CTX_SPILL + 24
.equ THREAD_CTX_S3, THREAD_CTX_SPILL + 32
.equ THREAD_CTX_S4, THREAD_CTX_SPILL + 40
.equ THREAD_CTX_S5, THREAD_CTX_SPILL + 48
.equ THREAD_CTX_S6, THREAD_CTX_SPILL + 56
.equ THREAD_CTX_S7, THREAD_CTX_SPILL + 64

.global loongarch64_thread_context_switch
loongarch64_thread_context_switch:
	st.d $fp, $a0, THREAD_CTX_FP
	st.d $s0, $a0, THREAD_CTX_S0
	st.d $s1, $a0, THREAD_CTX_S1
	st.d $s2, $a0, THREAD_CTX_S2
	st.d $s3, $a0, THREAD_CTX_S3
	st.d $s4, $a0, THREAD_CTX_S4
	st.d $s5, $a0, THREAD_CTX_S5
	st.d $s6, $a0, THREAD_CTX_S6
	st.d $s7, $a0, THREAD_CTX_S7
	st.d $sp, $a0, THREAD_CTX_SP
	la.local $t0, .Lresume
	st.d $t0, $a0, THREAD_CTX_IP

	ld.d $fp, $a1, THREAD_CTX_FP
	ld.d $s0, $a1, THREAD_CTX_S0
	ld.d $s1, $a1, THREAD_CTX_S1
	ld.d $s2, $a1, THREAD_CTX_S2
	ld.d $s3, $a1, THREAD_CTX_S3
	ld.d $s4, $a1, THREAD_CTX_S4
	ld.d $s5, $a1, THREAD_CTX_S5
	ld.d $s6, $a1, THREAD_CTX_S6
	ld.d $s7, $a1, THREAD_CTX_S7
	ld.d $sp, $a1, THREAD_CTX_SP
	ld.d $t0, $a1, THREAD_CTX_IP
	jirl $zero, $t0, 0

.Lresume:
	ret

.global loongarch64_thread_entry
loongarch64_thread_entry:
	addi.d $a0, $s1, 0
	jirl $zero, $s0, 0

.section .note.GNU-stack,"",@progbits
