.section .text

.global riscv64_userspace_enter
.extern riscv64_prepare_user_return
riscv64_userspace_enter:
	call riscv64_prepare_user_return
	csrw sepc, s0
	li t0, 0x100
	csrc sstatus, t0
	li t0, 0x20
	csrs sstatus, t0
	mv a0, s1
	mv sp, s2
	sret

.section .note.GNU-stack,"",@progbits
