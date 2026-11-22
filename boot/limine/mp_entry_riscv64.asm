.section .text
.global limine_mp_entry
.type limine_mp_entry, @function
limine_mp_entry:
	csrc sstatus, 2
	beqz a0, .Lpark
	ld t0, 32(a0)
	beqz t0, .Lpark
	ld sp, 24(t0)
	andi sp, sp, -16
	ld a0, 16(t0)
	ld a1, 8(t0)
	ld t0, 0(t0)
	beqz t0, .Lpark
	jalr t0
.Lpark:
	wfi
	j .Lpark
.size limine_mp_entry, . - limine_mp_entry

.section .note.GNU-stack,"",@progbits
