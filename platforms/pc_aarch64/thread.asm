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

.section .note.GNU-stack,"",%progbits
