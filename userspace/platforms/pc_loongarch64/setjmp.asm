.section .text

.global libc_setjmp
libc_setjmp:
	st.d $ra, $a0, 0
	st.d $sp, $a0, 8
	st.d $r21, $a0, 16
	st.d $fp, $a0, 24
	st.d $s0, $a0, 32
	st.d $s1, $a0, 40
	st.d $s2, $a0, 48
	st.d $s3, $a0, 56
	st.d $s4, $a0, 64
	st.d $s5, $a0, 72
	st.d $s6, $a0, 80
	st.d $s7, $a0, 88
	st.d $s8, $a0, 96
	fst.d $fs0, $a0, 104
	fst.d $fs1, $a0, 112
	fst.d $fs2, $a0, 120
	fst.d $fs3, $a0, 128
	fst.d $fs4, $a0, 136
	fst.d $fs5, $a0, 144
	fst.d $fs6, $a0, 152
	fst.d $fs7, $a0, 160
	addi.d $a0, $zero, 0
	jr $ra

.global longjmp
longjmp:
	addi.d $t0, $a0, 0
	bnez $a1, .Lrestore
	addi.d $a1, $zero, 1
.Lrestore:
	ld.d $ra, $t0, 0
	ld.d $sp, $t0, 8
	ld.d $r21, $t0, 16
	ld.d $fp, $t0, 24
	ld.d $s0, $t0, 32
	ld.d $s1, $t0, 40
	ld.d $s2, $t0, 48
	ld.d $s3, $t0, 56
	ld.d $s4, $t0, 64
	ld.d $s5, $t0, 72
	ld.d $s6, $t0, 80
	ld.d $s7, $t0, 88
	ld.d $s8, $t0, 96
	fld.d $fs0, $t0, 104
	fld.d $fs1, $t0, 112
	fld.d $fs2, $t0, 120
	fld.d $fs3, $t0, 128
	fld.d $fs4, $t0, 136
	fld.d $fs5, $t0, 144
	fld.d $fs6, $t0, 152
	fld.d $fs7, $t0, 160
	addi.d $a0, $a1, 0
	jr $ra

.section .note.GNU-stack,"",@progbits
