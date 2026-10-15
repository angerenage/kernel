.section .text

.global libc_setjmp
libc_setjmp:
	sd s0, 0(a0)
	sd s1, 8(a0)
	sd s2, 16(a0)
	sd s3, 24(a0)
	sd s4, 32(a0)
	sd s5, 40(a0)
	sd s6, 48(a0)
	sd s7, 56(a0)
	sd s8, 64(a0)
	sd s9, 72(a0)
	sd s10, 80(a0)
	sd s11, 88(a0)
	sd sp, 96(a0)
	sd ra, 104(a0)
	fsd f8, 112(a0)
	fsd f9, 120(a0)
	fsd f18, 128(a0)
	fsd f19, 136(a0)
	fsd f20, 144(a0)
	fsd f21, 152(a0)
	fsd f22, 160(a0)
	fsd f23, 168(a0)
	fsd f24, 176(a0)
	fsd f25, 184(a0)
	fsd f26, 192(a0)
	fsd f27, 200(a0)
	li a0, 0
	ret

.global longjmp
longjmp:
	bnez a1, .Lrestore
	li a1, 1
.Lrestore:
	ld s0, 0(a0)
	ld s1, 8(a0)
	ld s2, 16(a0)
	ld s3, 24(a0)
	ld s4, 32(a0)
	ld s5, 40(a0)
	ld s6, 48(a0)
	ld s7, 56(a0)
	ld s8, 64(a0)
	ld s9, 72(a0)
	ld s10, 80(a0)
	ld s11, 88(a0)
	ld sp, 96(a0)
	ld ra, 104(a0)
	fld f8, 112(a0)
	fld f9, 120(a0)
	fld f18, 128(a0)
	fld f19, 136(a0)
	fld f20, 144(a0)
	fld f21, 152(a0)
	fld f22, 160(a0)
	fld f23, 168(a0)
	fld f24, 176(a0)
	fld f25, 184(a0)
	fld f26, 192(a0)
	fld f27, 200(a0)
	mv a0, a1
	jr ra

.section .note.GNU-stack,"",%progbits
