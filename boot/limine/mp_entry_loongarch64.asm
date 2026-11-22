.section .text
.global limine_mp_entry
.type limine_mp_entry, @function
limine_mp_entry:
	csrrd $t1, 0x0
	bstrins.d $t1, $zero, 2, 2
	csrwr $t1, 0x0
	beqz $a0, .Lpark
	ld.d $t0, $a0, 32
	beqz $t0, .Lpark
	ld.d $sp, $t0, 24
	bstrins.d $sp, $zero, 3, 0
	ld.d $a0, $t0, 16
	ld.d $a1, $t0, 8
	ld.d $t0, $t0, 0
	beqz $t0, .Lpark
	jirl $ra, $t0, 0
.Lpark:
	idle 0
	b .Lpark
.size limine_mp_entry, . - limine_mp_entry

.section .note.GNU-stack,"",@progbits
