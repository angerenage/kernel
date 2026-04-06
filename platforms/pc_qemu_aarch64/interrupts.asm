.section .text
.global exception_vectors
.extern handle_exception
.extern aarch64_exception_stack_bottom
.extern aarch64_exception_stack_top

.equ AARCH64_EXCEPTION_FRAME_SIZE, 288
.equ AARCH64_EXCEPTION_META_SIZE, 16

.macro VECTOR_SLOT index
	b aarch64_vector_\index
	.space 124
.endm

.macro VECTOR_ENTRY index
aarch64_vector_\index:
	msr tpidr_el1, x16
	msr tpidrro_el0, x17

	mov x17, sp
	adrp x16, aarch64_exception_stack_bottom
	ldr x16, [x16, :lo12:aarch64_exception_stack_bottom]
	cmp x17, x16
	b.lo 0f
	adrp x16, aarch64_exception_stack_top
	ldr x16, [x16, :lo12:aarch64_exception_stack_top]
	cmp x17, x16
	b.hs 0f
	sub sp, sp, #AARCH64_EXCEPTION_META_SIZE
	str xzr, [sp]
	b 1f
0:
	adrp x16, aarch64_exception_stack_top
	ldr x16, [x16, :lo12:aarch64_exception_stack_top]
	mov sp, x16
	sub sp, sp, #AARCH64_EXCEPTION_META_SIZE
	str x17, [sp]
1:
	sub sp, sp, #AARCH64_EXCEPTION_FRAME_SIZE

	stp x0, x1, [sp, #0]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	mrs x16, tpidr_el1
	str x16, [sp, #128]
	mrs x16, tpidrro_el0
	str x16, [sp, #136]
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

	mov x0, sp
	bl handle_exception

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
	ldr x16, [sp]
	add sp, sp, #AARCH64_EXCEPTION_META_SIZE
	cbz x16, 2f
	mov sp, x16
2:
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
