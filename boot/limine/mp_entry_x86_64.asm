.intel_syntax noprefix

.section .text
.global limine_mp_entry
.type limine_mp_entry, @function
limine_mp_entry:
	cli
	test rdi, rdi
	jz .Lpark
	mov rax, qword ptr [rdi + 24]
	test rax, rax
	jz .Lpark
	mov rsp, qword ptr [rax + 24]
	and rsp, -16
	mov rdi, qword ptr [rax + 16]
	mov rsi, qword ptr [rax + 8]
	mov rax, qword ptr [rax]
	test rax, rax
	jz .Lpark
	call rax
.Lpark:
	cli
	hlt
	jmp .Lpark
.size limine_mp_entry, . - limine_mp_entry

.section .note.GNU-stack,"",@progbits
