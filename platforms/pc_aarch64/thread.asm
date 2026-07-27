.section .text

.equ THREAD_CTX_IP, 0
.equ THREAD_CTX_SP, 8
.equ THREAD_CTX_SPILL, 16
.equ THREAD_CTX_X19, THREAD_CTX_SPILL + 0
.equ THREAD_CTX_X20, THREAD_CTX_SPILL + 8
.equ THREAD_CTX_X21, THREAD_CTX_SPILL + 16
.equ THREAD_CTX_X22, THREAD_CTX_SPILL + 24
.equ THREAD_CTX_X23, THREAD_CTX_SPILL + 32
.equ THREAD_CTX_X24, THREAD_CTX_SPILL + 40
.equ THREAD_CTX_X25, THREAD_CTX_SPILL + 48
.equ THREAD_CTX_X26, THREAD_CTX_SPILL + 56
.equ THREAD_CTX_X27, THREAD_CTX_SPILL + 64
.equ THREAD_CTX_X28, THREAD_CTX_SPILL + 72
.equ THREAD_CTX_X29, THREAD_CTX_SPILL + 80

.global aarch64_thread_context_switch
aarch64_thread_context_switch:
	str x19, [x0, #THREAD_CTX_X19]
	str x20, [x0, #THREAD_CTX_X20]
	str x21, [x0, #THREAD_CTX_X21]
	str x22, [x0, #THREAD_CTX_X22]
	str x23, [x0, #THREAD_CTX_X23]
	str x24, [x0, #THREAD_CTX_X24]
	str x25, [x0, #THREAD_CTX_X25]
	str x26, [x0, #THREAD_CTX_X26]
	str x27, [x0, #THREAD_CTX_X27]
	str x28, [x0, #THREAD_CTX_X28]
	str x29, [x0, #THREAD_CTX_X29]
	mov x9, sp
	str x9, [x0, #THREAD_CTX_SP]
	adr x9, .Lresume
	str x9, [x0, #THREAD_CTX_IP]

	ldr x19, [x1, #THREAD_CTX_X19]
	ldr x20, [x1, #THREAD_CTX_X20]
	ldr x21, [x1, #THREAD_CTX_X21]
	ldr x22, [x1, #THREAD_CTX_X22]
	ldr x23, [x1, #THREAD_CTX_X23]
	ldr x24, [x1, #THREAD_CTX_X24]
	ldr x25, [x1, #THREAD_CTX_X25]
	ldr x26, [x1, #THREAD_CTX_X26]
	ldr x27, [x1, #THREAD_CTX_X27]
	ldr x28, [x1, #THREAD_CTX_X28]
	ldr x29, [x1, #THREAD_CTX_X29]
	ldr x9, [x1, #THREAD_CTX_SP]
	mov sp, x9
	ldr x9, [x1, #THREAD_CTX_IP]
	br x9

.Lresume:
	ret

.global aarch64_thread_entry
aarch64_thread_entry:
	mov x0, x20
	br x19


.global aarch64_fp_init
aarch64_fp_init:
	mrs x1, id_aa64pfr0_el1
	ubfx x2, x1, #16, #4
	cmp x2, #0xf
	b.eq .Laarch64_fp_unavailable
	ubfx x2, x1, #20, #4
	cmp x2, #0xf
	b.eq .Laarch64_fp_unavailable
	mrs x1, cpacr_el1
	orr x1, x1, #(3 << 20)
	msr cpacr_el1, x1
	isb
	msr fpcr, xzr
	msr fpsr, xzr
	mov w0, #1
	ret
.Laarch64_fp_unavailable:
	mov w0, #0
	ret

.global aarch64_fp_context_save
aarch64_fp_context_save:
	stp q0, q1, [x0, #0]
	stp q2, q3, [x0, #32]
	stp q4, q5, [x0, #64]
	stp q6, q7, [x0, #96]
	stp q8, q9, [x0, #128]
	stp q10, q11, [x0, #160]
	stp q12, q13, [x0, #192]
	stp q14, q15, [x0, #224]
	stp q16, q17, [x0, #256]
	stp q18, q19, [x0, #288]
	stp q20, q21, [x0, #320]
	stp q22, q23, [x0, #352]
	stp q24, q25, [x0, #384]
	stp q26, q27, [x0, #416]
	stp q28, q29, [x0, #448]
	stp q30, q31, [x0, #480]
	mrs x1, fpcr
	str w1, [x0, #512]
	mrs x1, fpsr
	str w1, [x0, #516]
	ret

.global aarch64_fp_context_restore
aarch64_fp_context_restore:
	ldp q0, q1, [x0, #0]
	ldp q2, q3, [x0, #32]
	ldp q4, q5, [x0, #64]
	ldp q6, q7, [x0, #96]
	ldp q8, q9, [x0, #128]
	ldp q10, q11, [x0, #160]
	ldp q12, q13, [x0, #192]
	ldp q14, q15, [x0, #224]
	ldp q16, q17, [x0, #256]
	ldp q18, q19, [x0, #288]
	ldp q20, q21, [x0, #320]
	ldp q22, q23, [x0, #352]
	ldp q24, q25, [x0, #384]
	ldp q26, q27, [x0, #416]
	ldp q28, q29, [x0, #448]
	ldp q30, q31, [x0, #480]
	ldr w1, [x0, #512]
	msr fpcr, x1
	ldr w1, [x0, #516]
	msr fpsr, x1
	ret

.section .note.GNU-stack,"",%progbits
