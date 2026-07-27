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


.global loongarch64_fp_init
loongarch64_fp_init:
	csrrd $t0, 0x2
	ori $t0, $t0, 7
	csrwr $t0, 0x2
	addi.d $a0, $zero, 1
	ret

.global loongarch64_fp_context_save
loongarch64_fp_context_save:
	xvst $xr0, $a0, 0
	xvst $xr1, $a0, 32
	xvst $xr2, $a0, 64
	xvst $xr3, $a0, 96
	xvst $xr4, $a0, 128
	xvst $xr5, $a0, 160
	xvst $xr6, $a0, 192
	xvst $xr7, $a0, 224
	xvst $xr8, $a0, 256
	xvst $xr9, $a0, 288
	xvst $xr10, $a0, 320
	xvst $xr11, $a0, 352
	xvst $xr12, $a0, 384
	xvst $xr13, $a0, 416
	xvst $xr14, $a0, 448
	xvst $xr15, $a0, 480
	xvst $xr16, $a0, 512
	xvst $xr17, $a0, 544
	xvst $xr18, $a0, 576
	xvst $xr19, $a0, 608
	xvst $xr20, $a0, 640
	xvst $xr21, $a0, 672
	xvst $xr22, $a0, 704
	xvst $xr23, $a0, 736
	xvst $xr24, $a0, 768
	xvst $xr25, $a0, 800
	xvst $xr26, $a0, 832
	xvst $xr27, $a0, 864
	xvst $xr28, $a0, 896
	xvst $xr29, $a0, 928
	xvst $xr30, $a0, 960
	xvst $xr31, $a0, 992
	movcf2gr $t0, $fcc0
	st.b $t0, $a0, 1024
	movcf2gr $t0, $fcc1
	st.b $t0, $a0, 1025
	movcf2gr $t0, $fcc2
	st.b $t0, $a0, 1026
	movcf2gr $t0, $fcc3
	st.b $t0, $a0, 1027
	movcf2gr $t0, $fcc4
	st.b $t0, $a0, 1028
	movcf2gr $t0, $fcc5
	st.b $t0, $a0, 1029
	movcf2gr $t0, $fcc6
	st.b $t0, $a0, 1030
	movcf2gr $t0, $fcc7
	st.b $t0, $a0, 1031
	movfcsr2gr $t0, $fcsr0
	st.w $t0, $a0, 1032
	ret

.global loongarch64_fp_context_restore
loongarch64_fp_context_restore:
	xvld $xr0, $a0, 0
	xvld $xr1, $a0, 32
	xvld $xr2, $a0, 64
	xvld $xr3, $a0, 96
	xvld $xr4, $a0, 128
	xvld $xr5, $a0, 160
	xvld $xr6, $a0, 192
	xvld $xr7, $a0, 224
	xvld $xr8, $a0, 256
	xvld $xr9, $a0, 288
	xvld $xr10, $a0, 320
	xvld $xr11, $a0, 352
	xvld $xr12, $a0, 384
	xvld $xr13, $a0, 416
	xvld $xr14, $a0, 448
	xvld $xr15, $a0, 480
	xvld $xr16, $a0, 512
	xvld $xr17, $a0, 544
	xvld $xr18, $a0, 576
	xvld $xr19, $a0, 608
	xvld $xr20, $a0, 640
	xvld $xr21, $a0, 672
	xvld $xr22, $a0, 704
	xvld $xr23, $a0, 736
	xvld $xr24, $a0, 768
	xvld $xr25, $a0, 800
	xvld $xr26, $a0, 832
	xvld $xr27, $a0, 864
	xvld $xr28, $a0, 896
	xvld $xr29, $a0, 928
	xvld $xr30, $a0, 960
	xvld $xr31, $a0, 992
	ld.bu $t0, $a0, 1024
	movgr2cf $fcc0, $t0
	ld.bu $t0, $a0, 1025
	movgr2cf $fcc1, $t0
	ld.bu $t0, $a0, 1026
	movgr2cf $fcc2, $t0
	ld.bu $t0, $a0, 1027
	movgr2cf $fcc3, $t0
	ld.bu $t0, $a0, 1028
	movgr2cf $fcc4, $t0
	ld.bu $t0, $a0, 1029
	movgr2cf $fcc5, $t0
	ld.bu $t0, $a0, 1030
	movgr2cf $fcc6, $t0
	ld.bu $t0, $a0, 1031
	movgr2cf $fcc7, $t0
	ld.w $t0, $a0, 1032
	movgr2fcsr $fcsr0, $t0
	ret

.section .note.GNU-stack,"",@progbits
