BITS 64

SECTION .text
GLOBAL _start

extern kernel_main

_start:
	lea rsp, [rel stack_top]
	mov rbp, rsp

	call kernel_main

.hang:
	hlt
	jmp .hang

SECTION .bss
align 16
stack_bottom:
	resb 0x4000
stack_top:

SECTION .note.GNU-stack noalloc nobits
