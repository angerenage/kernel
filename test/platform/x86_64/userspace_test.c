#include <criterion/criterion.h>
#include <hal/userspace.h>
#include <stdint.h>
#include <string.h>

#include "../../../platforms/pc_x86_64/hal/interrupts_private.h"

#define X86_RFLAGS_DIRECTION (1ull << 10)

static struct hal_cpu_fp_context live_fp_context;

bool hal_cpu_fp_init(void) {
	hal_cpu_fp_context_init(&live_fp_context);
	return true;
}

void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	memset(context, 0, sizeof(*context));
}

void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	*context = live_fp_context;
}

void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	live_fp_context = *context;
}

Test(x86_64_userspace, exposes_aligned_fp_context_storage) {
	struct hal_cpu_fp_context context;
	uintptr_t                 base = (uintptr_t)&context;
	uintptr_t                 data = (uintptr_t)hal_cpu_fp_context_data(&context);

	cr_assert_eq(data & (HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u), 0u);
	cr_assert(data >= base);
	cr_assert(data + HAL_CPU_FP_CONTEXT_SIZE <= base + sizeof(context));
}

void x86_64_userspace_enter(void) {
}

static struct user_interrupt_frame user_frame(void) {
	return (struct user_interrupt_frame){
		.frame =
			{
					.rax    = 0x01u,
					.rbx    = 0x02u,
					.rcx    = 0x03u,
					.rdx    = 0x04u,
					.rbp    = 0x05u,
					.rdi    = 0x06u,
					.rsi    = 0x07u,
					.r8     = 0x08u,
					.r9     = 0x09u,
					.r10    = 0x0au,
					.r11    = 0x0bu,
					.r12    = 0x0cu,
					.r13    = 0x0du,
					.r14    = 0x0eu,
					.r15    = 0x0fu,
					.vector = 0x80u,
					.rip    = 0x1000u,
					.cs     = X86_GDT_USER_CODE_SELECTOR | 0x3u,
					.rflags = 0x202u,
					},
		.rsp = 0x8000u,
		.ss  = X86_GDT_USER_DATA_SELECTOR | 0x3u,
	};
}

static struct hal_userspace_return_frame* opaque_frame(struct user_interrupt_frame* frame) {
	return (struct hal_userspace_return_frame*)frame;
}

static const struct hal_userspace_return_frame* opaque_const_frame(const struct user_interrupt_frame* frame) {
	return (const struct hal_userspace_return_frame*)frame;
}

Test(x86_64_userspace, recognizes_only_complete_user_frames) {
	struct user_interrupt_frame frame = user_frame();

	cr_assert(hal_userspace_frame_is_user(opaque_const_frame(&frame)));
	frame.frame.cs = 0x08u;
	cr_assert_not(hal_userspace_frame_is_user(opaque_const_frame(&frame)));
	frame    = user_frame();
	frame.ss = 0x10u;
	cr_assert_not(hal_userspace_frame_is_user(opaque_const_frame(&frame)));
	cr_assert_not(hal_userspace_frame_is_user(NULL));
}

Test(x86_64_userspace, saves_and_restores_the_complete_user_context) {
	struct user_interrupt_frame  original = user_frame();
	struct user_interrupt_frame  frame    = original;
	struct hal_userspace_context context;
	struct hal_cpu_fp_context    original_fp;

	memset(&live_fp_context, 0x5a, sizeof(live_fp_context));
	original_fp = live_fp_context;
	cr_assert(hal_userspace_context_save(&context, opaque_const_frame(&frame)));
	memset(&frame, 0xa5, sizeof(frame));
	memset(&live_fp_context, 0xa5, sizeof(live_fp_context));
	frame.frame.cs = X86_GDT_USER_CODE_SELECTOR | 0x3u;
	frame.ss       = X86_GDT_USER_DATA_SELECTOR | 0x3u;
	cr_assert(hal_userspace_context_restore(opaque_frame(&frame), &context));
	cr_assert_eq(memcmp(&frame, &original, sizeof(frame)), 0);
	cr_assert_eq(memcmp(&live_fp_context, &original_fp, sizeof(original_fp)), 0);
}

Test(x86_64_userspace, rejects_non_user_saved_contexts) {
	struct user_interrupt_frame  frame = user_frame();
	struct hal_userspace_context context;

	memset(&context, 0, sizeof(context));
	cr_assert_not(hal_userspace_context_restore(opaque_frame(&frame), &context));
	frame.frame.cs = 0x08u;
	cr_assert_not(hal_userspace_context_save(&context, opaque_const_frame(&frame)));
}

Test(x86_64_userspace, redirects_with_sysv_stack_shape_and_safe_direction_flag) {
	struct user_interrupt_frame before = user_frame();
	struct user_interrupt_frame frame;

	before.frame.rflags |= X86_RFLAGS_DIRECTION;
	frame = before;

	cr_assert(hal_userspace_frame_redirect(opaque_frame(&frame), 0x4000u, 0x9000u, 0x11u, 0x22u, 0x33u, 0x44u));
	cr_assert_eq(frame.frame.rip, 0x4000u);
	cr_assert_eq(frame.rsp, 0x8ff8u);
	cr_assert_eq((frame.rsp + sizeof(uintptr_t)) & (HAL_USERSPACE_STACK_ALIGNMENT - 1u), 0u);
	cr_assert_eq(frame.frame.rdi, 0x11u);
	cr_assert_eq(frame.frame.rsi, 0x22u);
	cr_assert_eq(frame.frame.rdx, 0x33u);
	cr_assert_eq(frame.frame.rcx, 0x44u);
	cr_assert_eq(frame.frame.rflags & X86_RFLAGS_DIRECTION, 0u);

	frame.frame.rip    = before.frame.rip;
	frame.rsp          = before.rsp;
	frame.frame.rdi    = before.frame.rdi;
	frame.frame.rsi    = before.frame.rsi;
	frame.frame.rdx    = before.frame.rdx;
	frame.frame.rcx    = before.frame.rcx;
	frame.frame.rflags = before.frame.rflags;
	cr_assert_eq(memcmp(&frame, &before, sizeof(frame)), 0);
}

Test(x86_64_userspace, rejects_invalid_redirect_targets) {
	struct user_interrupt_frame frame = user_frame();

	cr_assert_not(hal_userspace_frame_redirect(opaque_frame(&frame), 0u, 0x9000u, 0u, 0u, 0u, 0u));
	cr_assert_not(hal_userspace_frame_redirect(opaque_frame(&frame), 0x4000u, 0u, 0u, 0u, 0u, 0u));
	cr_assert_not(hal_userspace_frame_redirect(opaque_frame(&frame), 0x4000u, 0x9008u, 0u, 0u, 0u, 0u));
	frame.frame.cs = 0x08u;
	cr_assert_not(hal_userspace_frame_redirect(opaque_frame(&frame), 0x4000u, 0x9000u, 0u, 0u, 0u, 0u));
}
