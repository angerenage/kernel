.section .text
.global exception_vectors
.extern handle_exception
.extern aarch64_maybe_preempt_on_interrupt_exit

.equ AARCH64_EXCEPTION_FRAME_SIZE, 304

.macro VECTOR_SLOT index
	b aarch64_vector_\index
	.space 124
.endm

.macro VECTOR_ENTRY index
aarch64_vector_\index:
	/*
	 * Lower-EL exceptions arrive on SP_EL1. Current-EL exceptions already
	 * use the interrupted kernel stack. Keeping the frame there is required
	 * because a scheduler context switch may suspend this handler.
	 */
	sub sp, sp, #16
	stp x16, x17, [sp]
	sub sp, sp, #AARCH64_EXCEPTION_FRAME_SIZE

	stp x0, x1, [sp, #0]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #AARCH64_EXCEPTION_FRAME_SIZE]
	str x16, [sp, #128]
	str x17, [sp, #136]
	stp x18, x19, [sp, #144]
	stp x20, x21, [sp, #160]
	stp x22, x23, [sp, #176]
	stp x24, x25, [sp, #192]
	stp x26, x27, [sp, #208]
	stp x28, x29, [sp, #224]
	str x30, [sp, #240]

	mov x16, #\index
	str x16, [sp, #248]

	mrs x16, esr_el1
	str x16, [sp, #256]
	mrs x16, far_el1
	str x16, [sp, #264]
	mrs x16, elr_el1
	str x16, [sp, #272]
	mrs x16, spsr_el1
	str x16, [sp, #280]
	mrs x16, sp_el0
	str x16, [sp, #288]
	str xzr, [sp, #296]

	mov x0, sp
	bl handle_exception
	bl aarch64_maybe_preempt_on_interrupt_exit

	ldr x16, [sp, #272]
	msr elr_el1, x16
	ldr x16, [sp, #280]
	msr spsr_el1, x16
	ldr x16, [sp, #288]
	msr sp_el0, x16
	isb

	ldp x0, x1, [sp, #0]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldr x16, [sp, #128]
	ldr x17, [sp, #136]
	ldp x18, x19, [sp, #144]
	ldp x20, x21, [sp, #160]
	ldp x22, x23, [sp, #176]
	ldp x24, x25, [sp, #192]
	ldp x26, x27, [sp, #208]
	ldp x28, x29, [sp, #224]
	ldr x30, [sp, #240]

	add sp, sp, #AARCH64_EXCEPTION_FRAME_SIZE
	ldp x16, x17, [sp]
	add sp, sp, #16
	eret
.endm

.balign 2048
exception_vectors:
	VECTOR_SLOT 0
	VECTOR_SLOT 1
	VECTOR_SLOT 2
	VECTOR_SLOT 3
	VECTOR_SLOT 4
	VECTOR_SLOT 5
	VECTOR_SLOT 6
	VECTOR_SLOT 7
	VECTOR_SLOT 8
	VECTOR_SLOT 9
	VECTOR_SLOT 10
	VECTOR_SLOT 11
	VECTOR_SLOT 12
	VECTOR_SLOT 13
	VECTOR_SLOT 14
	VECTOR_SLOT 15

VECTOR_ENTRY 0
VECTOR_ENTRY 1
VECTOR_ENTRY 2
VECTOR_ENTRY 3
VECTOR_ENTRY 4
VECTOR_ENTRY 5
VECTOR_ENTRY 6
VECTOR_ENTRY 7
VECTOR_ENTRY 8
VECTOR_ENTRY 9
VECTOR_ENTRY 10
VECTOR_ENTRY 11
VECTOR_ENTRY 12
VECTOR_ENTRY 13
VECTOR_ENTRY 14
VECTOR_ENTRY 15

.section .note.GNU-stack,"",%progbits
