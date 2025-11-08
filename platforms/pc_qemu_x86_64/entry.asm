BITS 64

SECTION .text
GLOBAL _start

extern kernel_main

_start:
	lea rsp, [rel stack_top]
	mov rbp, rsp

	; Enable SSE/FPU usage before calling into C.
	mov rax, cr0
	and eax, ~0x4              ; clear EM bit to allow FPU/SSE instructions
	or eax, 0x22               ; set MP and NE bits
	mov cr0, rax

	mov rax, cr4
	or eax, (1 << 9) | (1 << 10) ; enable OSFXSR and OSXMMEXCPT
	mov cr4, rax

	finit

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
