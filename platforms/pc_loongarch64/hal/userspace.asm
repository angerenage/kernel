.section .text

.global loongarch64_userspace_enter
.extern loongarch64_prepare_user_return
.extern sched_complete_context_switch
loongarch64_userspace_enter:
	bl sched_complete_context_switch
	bl loongarch64_prepare_user_return
	csrwr $s0, 0x6
	ori $t0, $zero, 0x7
	csrwr $t0, 0x1
	addi.d $a0, $s1, 0
	addi.d $sp, $s2, 0
	ertn

.section .note.GNU-stack,"",@progbits
