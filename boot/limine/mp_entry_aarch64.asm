.section .text
.global limine_mp_entry
.type limine_mp_entry, %function
limine_mp_entry:
	msr daifset, #0xf
	cbz x0, .Lpark
	ldr x9, [x0, #32]
	cbz x9, .Lpark
	ldr x10, [x9, #24]
	and x10, x10, #-16
	mov sp, x10
	ldr x0, [x9, #16]
	ldr x1, [x9, #8]
	ldr x9, [x9]
	cbz x9, .Lpark
	blr x9
.Lpark:
	wfe
	b .Lpark
.size limine_mp_entry, . - limine_mp_entry

.section .note.GNU-stack,"",%progbits
