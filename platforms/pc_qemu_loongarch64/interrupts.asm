.section .text
.global loongarch64_exception_entry
.extern loongarch64_handle_exception

.equ LOONGARCH64_EXCEPTION_FRAME_SIZE, 288

.balign 4096
loongarch64_exception_entry:
	addi.d $sp, $sp, -LOONGARCH64_EXCEPTION_FRAME_SIZE

	st.d $r0, $sp, 0
	st.d $r1, $sp, 8
	st.d $r2, $sp, 16
	st.d $r12, $sp, 96
	addi.d $r12, $sp, LOONGARCH64_EXCEPTION_FRAME_SIZE
	st.d $r12, $sp, 24
	st.d $r4, $sp, 32
	st.d $r5, $sp, 40
	st.d $r6, $sp, 48
	st.d $r7, $sp, 56
	st.d $r8, $sp, 64
	st.d $r9, $sp, 72
	st.d $r10, $sp, 80
	st.d $r11, $sp, 88
	st.d $r13, $sp, 104
	st.d $r14, $sp, 112
	st.d $r15, $sp, 120
	st.d $r16, $sp, 128
	st.d $r17, $sp, 136
	st.d $r18, $sp, 144
	st.d $r19, $sp, 152
	st.d $r20, $sp, 160
	st.d $r21, $sp, 168
	st.d $r22, $sp, 176
	st.d $r23, $sp, 184
	st.d $r24, $sp, 192
	st.d $r25, $sp, 200
	st.d $r26, $sp, 208
	st.d $r27, $sp, 216
	st.d $r28, $sp, 224
	st.d $r29, $sp, 232
	st.d $r30, $sp, 240
	st.d $r31, $sp, 248

	csrrd $r12, 0x5
	st.d $r12, $sp, 256
	csrrd $r12, 0x6
	st.d $r12, $sp, 264
	csrrd $r12, 0x7
	st.d $r12, $sp, 272
	st.d $r0, $sp, 280

	addi.d $a0, $sp, 0
	bl loongarch64_handle_exception

1:
	b 1b

.section .note.GNU-stack,"",@progbits
