.section .text
.global exception_vectors
.extern handle_exception

.equ AARCH64_EXCEPTION_FRAME_SIZE, 288

.macro VECTOR_SLOT index
	b aarch64_vector_\index
	.space 124
.endm

.macro VECTOR_ENTRY index
aarch64_vector_\index:
	sub sp, sp, #16
	str x0, [sp, #8]
	mov x0, #\index
	str x0, [sp]
	ldr x0, [sp, #8]
	b aarch64_exception_common
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

aarch64_exception_common:
	sub sp, sp, #AARCH64_EXCEPTION_FRAME_SIZE

	stp x1, x2, [sp, #8]
	stp x3, x4, [sp, #24]
	stp x5, x6, [sp, #40]
	stp x7, x8, [sp, #56]
	stp x9, x10, [sp, #72]
	stp x11, x12, [sp, #88]
	stp x13, x14, [sp, #104]
	stp x15, x16, [sp, #120]
	stp x17, x18, [sp, #136]
	stp x19, x20, [sp, #152]
	stp x21, x22, [sp, #168]
	stp x23, x24, [sp, #184]
	stp x25, x26, [sp, #200]
	stp x27, x28, [sp, #216]
	stp x29, x30, [sp, #232]

	ldr x1, [sp, #AARCH64_EXCEPTION_FRAME_SIZE + 8]
	str x1, [sp]
	ldr x1, [sp, #AARCH64_EXCEPTION_FRAME_SIZE]
	str x1, [sp, #248]

	mrs x1, esr_el1
	str x1, [sp, #256]
	mrs x1, far_el1
	str x1, [sp, #264]
	mrs x1, elr_el1
	str x1, [sp, #272]
	mrs x1, spsr_el1
	str x1, [sp, #280]

	mov x0, sp
	bl handle_exception

	ldp x1, x2, [sp, #8]
	ldp x3, x4, [sp, #24]
	ldp x5, x6, [sp, #40]
	ldp x7, x8, [sp, #56]
	ldp x9, x10, [sp, #72]
	ldp x11, x12, [sp, #88]
	ldp x13, x14, [sp, #104]
	ldp x15, x16, [sp, #120]
	ldp x17, x18, [sp, #136]
	ldp x19, x20, [sp, #152]
	ldp x21, x22, [sp, #168]
	ldp x23, x24, [sp, #184]
	ldp x25, x26, [sp, #200]
	ldp x27, x28, [sp, #216]
	ldp x29, x30, [sp, #232]
	ldr x0, [sp]

	add sp, sp, #AARCH64_EXCEPTION_FRAME_SIZE
	add sp, sp, #16
	eret

.section .note.GNU-stack,"",%progbits
