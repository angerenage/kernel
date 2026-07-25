#include <core/exception.h>
#include <core/mm.h>
#include <core/syscall.h>
#include <criterion/criterion.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../../platforms/pc_x86_64/hal/interrupts_private.h"

static uintptr_t dispatched_number;
static uintptr_t dispatched_args[6];

void x86_64_syscall_entry(void) {
}

bool core_handle_user_exception(enum core_exception_kind kind) {
	(void)kind;
	return false;
}

__attribute__((noreturn))
void hcf(void) {
	abort();
}

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	dispatched_number  = number;
	dispatched_args[0] = arg0;
	dispatched_args[1] = arg1;
	dispatched_args[2] = arg2;
	dispatched_args[3] = arg3;
	dispatched_args[4] = arg4;
	dispatched_args[5] = arg5;
	return syscall_result_error(SYSCALL_STATUS_FAILED, 0xfeedu);
}

static struct user_interrupt_frame valid_user_frame(void) {
	const uint64_t rip    = MM_USER_VMM_BASE + 0x2000u;
	const uint64_t rsp    = MM_USER_VMM_BASE + 0x8000u;
	const uint64_t rflags = 0x202u;

	return (struct user_interrupt_frame){
		.frame =
			{
					.rcx    = rip,
					.r11    = rflags,
					.rip    = rip,
					.cs     = X86_GDT_USER_CODE_SELECTOR | 0x3u,
					.rflags = rflags,
					},
		.rsp = rsp,
		.ss  = X86_GDT_USER_DATA_SELECTOR | 0x3u,
	};
}

Test(x86_64_syscall, dispatches_six_arguments_from_the_normalized_frame) {
	struct interrupt_frame frame = {
		.rax    = 0x10u,
		.rdi    = 0x20u,
		.rsi    = 0x30u,
		.rdx    = 0x40u,
		.r10    = 0x50u,
		.r8     = 0x60u,
		.r9     = 0x70u,
		.vector = X86_SYSCALL_VECTOR,
	};

	cr_assert(x86_64_handle_syscall(&frame));
	cr_assert_eq(dispatched_number, 0x10u);
	cr_assert_eq(dispatched_args[0], 0x20u);
	cr_assert_eq(dispatched_args[1], 0x30u);
	cr_assert_eq(dispatched_args[2], 0x40u);
	cr_assert_eq(dispatched_args[3], 0x50u);
	cr_assert_eq(dispatched_args[4], 0x60u);
	cr_assert_eq(dispatched_args[5], 0x70u);
	cr_assert_eq(frame.rax, 0xfeedu);
	cr_assert_eq(frame.rdx, SYSCALL_STATUS_FAILED);
}

Test(x86_64_syscall, ignores_non_syscall_vectors) {
	struct interrupt_frame frame = {.vector = 14u, .rax = 0x1234u};

	cr_assert_not(x86_64_handle_syscall(&frame));
	cr_assert_eq(frame.rax, 0x1234u);
}

Test(x86_64_syscall, classifies_an_unmodified_syscall_frame_for_sysret) {
	struct user_interrupt_frame frame = valid_user_frame();

	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_SYSRET);
}

Test(x86_64_syscall, uses_iret_when_rcx_cannot_represent_the_saved_register) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.rcx++;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_IRET);
}

Test(x86_64_syscall, uses_iret_when_r11_cannot_represent_the_saved_register) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.r11 ^= 1u;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_IRET);
}

Test(x86_64_syscall, uses_iret_to_restore_resume_flag_precisely) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.rflags |= 1ull << 16;
	frame.frame.r11 = frame.frame.rflags;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_IRET);
}

Test(x86_64_syscall, rejects_kernel_or_noncanonical_return_addresses) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.rip = 0xffffffff80000000ull;
	frame.frame.rcx = frame.frame.rip;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_INVALID);
}

Test(x86_64_syscall, rejects_unexpected_user_selectors) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.cs = X86_GDT_USER_COMPAT_SELECTOR | 0x3u;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_INVALID);
}

Test(x86_64_syscall, rejects_privileged_rflags_bits) {
	struct user_interrupt_frame frame = valid_user_frame();

	frame.frame.rflags |= 3ull << 12;
	frame.frame.r11 = frame.frame.rflags;
	cr_assert_eq(x86_64_classify_user_return(&frame), X86_USER_RETURN_INVALID);
}
