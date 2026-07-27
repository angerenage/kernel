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


.global riscv64_fp_init
riscv64_fp_init:
	li t0, 0x6600
	csrs sstatus, t0
	csrw fcsr, zero
	csrw vstart, zero
	csrw vcsr, zero
	csrr t0, vlenb
	li t1, 16
	xor t0, t0, t1
	seqz a0, t0
	ret

.global riscv64_fp_context_save
riscv64_fp_context_save:
	fsd f0, 0(a0)
	fsd f1, 8(a0)
	fsd f2, 16(a0)
	fsd f3, 24(a0)
	fsd f4, 32(a0)
	fsd f5, 40(a0)
	fsd f6, 48(a0)
	fsd f7, 56(a0)
	fsd f8, 64(a0)
	fsd f9, 72(a0)
	fsd f10, 80(a0)
	fsd f11, 88(a0)
	fsd f12, 96(a0)
	fsd f13, 104(a0)
	fsd f14, 112(a0)
	fsd f15, 120(a0)
	fsd f16, 128(a0)
	fsd f17, 136(a0)
	fsd f18, 144(a0)
	fsd f19, 152(a0)
	fsd f20, 160(a0)
	fsd f21, 168(a0)
	fsd f22, 176(a0)
	fsd f23, 184(a0)
	fsd f24, 192(a0)
	fsd f25, 200(a0)
	fsd f26, 208(a0)
	fsd f27, 216(a0)
	fsd f28, 224(a0)
	fsd f29, 232(a0)
	fsd f30, 240(a0)
	fsd f31, 248(a0)
	csrr t0, fcsr
	sd t0, 256(a0)
	csrr t0, vstart
	sd t0, 264(a0)
	csrr t0, vcsr
	sd t0, 272(a0)
	csrr t0, vl
	sd t0, 280(a0)
	csrr t0, vtype
	sd t0, 288(a0)
	csrw vstart, zero
	addi t0, a0, 320
	vs8r.v v0, (t0)
	addi t0, t0, 128
	vs8r.v v8, (t0)
	addi t0, t0, 128
	vs8r.v v16, (t0)
	addi t0, t0, 128
	vs8r.v v24, (t0)
	ret

.global riscv64_fp_context_restore
riscv64_fp_context_restore:
	fld f0, 0(a0)
	fld f1, 8(a0)
	fld f2, 16(a0)
	fld f3, 24(a0)
	fld f4, 32(a0)
	fld f5, 40(a0)
	fld f6, 48(a0)
	fld f7, 56(a0)
	fld f8, 64(a0)
	fld f9, 72(a0)
	fld f10, 80(a0)
	fld f11, 88(a0)
	fld f12, 96(a0)
	fld f13, 104(a0)
	fld f14, 112(a0)
	fld f15, 120(a0)
	fld f16, 128(a0)
	fld f17, 136(a0)
	fld f18, 144(a0)
	fld f19, 152(a0)
	fld f20, 160(a0)
	fld f21, 168(a0)
	fld f22, 176(a0)
	fld f23, 184(a0)
	fld f24, 192(a0)
	fld f25, 200(a0)
	fld f26, 208(a0)
	fld f27, 216(a0)
	fld f28, 224(a0)
	fld f29, 232(a0)
	fld f30, 240(a0)
	fld f31, 248(a0)
	ld t0, 256(a0)
	csrw fcsr, t0
	ld t1, 264(a0)
	ld t2, 272(a0)
	ld t3, 280(a0)
	ld t4, 288(a0)
	csrw vstart, zero
	addi t0, a0, 320
	vl8re8.v v0, (t0)
	addi t0, t0, 128
	vl8re8.v v8, (t0)
	addi t0, t0, 128
	vl8re8.v v16, (t0)
	addi t0, t0, 128
	vl8re8.v v24, (t0)
	csrw vcsr, t2
	vsetvl zero, t3, t4
	csrw vstart, t1
	ret

.section .note.GNU-stack,"",@progbits
