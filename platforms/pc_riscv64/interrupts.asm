.section .text
.global exception_entry
.extern handle_exception
.extern riscv64_maybe_preempt_on_interrupt_exit
.extern riscv64_prepare_user_return
.extern core_finalize_user_return

.equ RISCV64_EXCEPTION_FRAME_SIZE, 288
.equ RISCV64_EXCEPTION_META_SIZE, 8
.equ RISCV64_EXCEPTION_STATE_KERNEL_STACK_TOP, 32
.equ RISCV64_SSTATUS_SPP, 0x100

.balign 4
exception_entry:
	csrrw t0, sscratch, t0
	sd sp, 0(t0)
	sd t1, 8(t0)
	sd t2, 16(t0)
	csrr t1, sscratch
	sd t1, 24(t0)
	csrw sscratch, t0

	/* S-mode traps stay on the interrupted kernel stack. */
	csrr t1, sstatus
	andi t1, t1, RISCV64_SSTATUS_SPP
	bnez t1, .Lriscv64_same_stack

	/* U-mode traps enter on the current thread's dedicated kernel stack. */
	ld sp, RISCV64_EXCEPTION_STATE_KERNEL_STACK_TOP(t0)
	addi sp, sp, -RISCV64_EXCEPTION_META_SIZE
	ld t1, 0(t0)
	sd t1, 0(sp)
	j .Lriscv64_frame

.Lriscv64_same_stack:
	ld sp, 0(t0)
	addi sp, sp, -RISCV64_EXCEPTION_META_SIZE
	sd zero, 0(sp)

.Lriscv64_frame:
	addi sp, sp, -RISCV64_EXCEPTION_FRAME_SIZE

	sd ra, 0(sp)
	ld t1, RISCV64_EXCEPTION_FRAME_SIZE + 0(sp)
	sd t1, 8(sp)
	sd gp, 16(sp)
	sd tp, 24(sp)
	ld t1, 24(t0)
	sd t1, 32(sp)
	ld t1, 8(t0)
	sd t1, 40(sp)
	ld t1, 16(t0)
	sd t1, 48(sp)
	sd s0, 56(sp)
	sd s1, 64(sp)
	sd a0, 72(sp)
	sd a1, 80(sp)
	sd a2, 88(sp)
	sd a3, 96(sp)
	sd a4, 104(sp)
	sd a5, 112(sp)
	sd a6, 120(sp)
	sd a7, 128(sp)
	sd s2, 136(sp)
	sd s3, 144(sp)
	sd s4, 152(sp)
	sd s5, 160(sp)
	sd s6, 168(sp)
	sd s7, 176(sp)
	sd s8, 184(sp)
	sd s9, 192(sp)
	sd s10, 200(sp)
	sd s11, 208(sp)
	sd t3, 216(sp)
	sd t4, 224(sp)
	sd t5, 232(sp)
	sd t6, 240(sp)

	csrr t0, scause
	sd t0, 248(sp)
	csrr t0, sepc
	sd t0, 256(sp)
	csrr t0, stval
	sd t0, 264(sp)
	csrr t0, sstatus
	sd t0, 272(sp)
	sd zero, 280(sp)

	mv a0, sp
	call handle_exception
	call riscv64_maybe_preempt_on_interrupt_exit
	call riscv64_prepare_user_return
	mv a0, sp
	call core_finalize_user_return

	ld ra, 0(sp)
	ld gp, 16(sp)
	ld tp, 24(sp)
	ld s0, 56(sp)
	ld s1, 64(sp)
	ld a0, 72(sp)
	ld a1, 80(sp)
	ld a2, 88(sp)
	ld a3, 96(sp)
	ld a4, 104(sp)
	ld a5, 112(sp)
	ld a6, 120(sp)
	ld a7, 128(sp)
	ld s2, 136(sp)
	ld s3, 144(sp)
	ld s4, 152(sp)
	ld s5, 160(sp)
	ld s6, 168(sp)
	ld s7, 176(sp)
	ld s8, 184(sp)
	ld s9, 192(sp)
	ld s10, 200(sp)
	ld s11, 208(sp)
	csrr t3, sscratch
	ld t1, 32(sp)
	sd t1, 24(t3)
	ld t0, 272(sp)
	csrw sstatus, t0
	andi t0, t0, RISCV64_SSTATUS_SPP
	bnez t0, .Lriscv64_kernel_return_stack
	ld t0, 8(sp)
	j .Lriscv64_return_stack_ready

.Lriscv64_kernel_return_stack:
	addi t0, sp, RISCV64_EXCEPTION_FRAME_SIZE + RISCV64_EXCEPTION_META_SIZE

.Lriscv64_return_stack_ready:
	ld t1, 256(sp)
	csrw sepc, t1
	ld t1, 40(sp)
	ld t2, 48(sp)
	ld t3, 216(sp)
	ld t4, 224(sp)
	ld t5, 232(sp)
	ld t6, 240(sp)

	mv sp, t0
	csrr t0, sscratch
	ld t0, 24(t0)
	sret

.section .note.GNU-stack,"",@progbits
