#include <stddef.h>
#include <stdint.h>

#include <hal/paging.h>
#include <hal/userspace.h>
#include <core/mm.h>

#include <criterion/criterion.h>

#define TEST_KERNEL_STACK_TOP 0x10000u
#define TEST_USER_ENTRY 0x4000u
#define TEST_USER_STACK 0x8000u

#if defined(__aarch64__)
#include "../../platforms/pc_aarch64/hal/interrupts/frame.h"
void aarch64_userspace_enter(void) {
}
#elif defined(__riscv)
#include "../../platforms/pc_riscv64/hal/interrupts/frame.h"
void riscv64_userspace_enter(void) {
}
#elif defined(__loongarch64)
#include "../../platforms/pc_loongarch64/hal/interrupts/frame.h"
void loongarch64_userspace_enter(void) {
}
#endif

const struct hal_paging_info* hal_paging_info(void) {
	static const struct hal_paging_info info = {.minimum_leaf_size = 0x1000u};
	return &info;
}

static struct exception_frame userspace_return_frame(void) {
#if defined(__aarch64__)
	return (struct exception_frame){.elr = TEST_USER_ENTRY, .sp_el0 = TEST_USER_STACK};
#elif defined(__riscv)
	return (struct exception_frame){.sp = TEST_USER_STACK, .sepc = TEST_USER_ENTRY, .sstatus = 1u << 5};
#elif defined(__loongarch64)
	struct exception_frame frame = {.era = TEST_USER_ENTRY, .prmd = 0x7u};
	frame.gpr[3] = TEST_USER_STACK;
	return frame;
#endif
}

static void userspace_return_set_entry(struct exception_frame* frame, uintptr_t entry) {
#if defined(__aarch64__)
	frame->elr = entry;
#elif defined(__riscv)
	frame->sepc = entry;
#elif defined(__loongarch64)
	frame->era = entry;
#endif
}

static void userspace_return_set_stack(struct exception_frame* frame, uintptr_t stack) {
#if defined(__aarch64__)
	frame->sp_el0 = stack;
#elif defined(__riscv)
	frame->sp = stack;
#elif defined(__loongarch64)
	frame->gpr[3] = stack;
#endif
}

static void userspace_return_set_privileged(struct exception_frame* frame) {
#if defined(__aarch64__)
	frame->spsr = 1u;
#elif defined(__riscv)
	frame->sstatus |= 1u << 8;
#elif defined(__loongarch64)
	frame->prmd = 0x4u;
#endif
}

static void userspace_return_set_unsafe_flags(struct exception_frame* frame) {
#if defined(__aarch64__)
	frame->spsr = 1u << 7;
#elif defined(__riscv)
	frame->sstatus |= 1u << 18;
#elif defined(__loongarch64)
	frame->prmd |= 1u << 8;
#endif
}

void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context) {
	(void)context;
}

void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context) {
	(void)context;
}

void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context) {
	(void)context;
}

Test(userspace_entry, rejects_null_and_misaligned_entries) {
	cr_assert(hal_userspace_entry_valid(0x4000u));
	cr_assert_not(hal_userspace_entry_valid(0u));
	cr_assert_not(hal_userspace_entry_valid(0x4001u));
}

Test(userspace_entry, initial_context_uses_the_entry_alignment_rule) {
	struct thread_context context;

	cr_assert(hal_userspace_thread_context_init(
			&context, TEST_KERNEL_STACK_TOP, TEST_USER_ENTRY, TEST_USER_STACK, 0x11u));
	cr_assert_not(hal_userspace_thread_context_init(
			&context, TEST_KERNEL_STACK_TOP, TEST_USER_ENTRY + 1u, TEST_USER_STACK, 0x11u));
}

Test(userspace_entry, validates_final_return_control_state) {
	struct exception_frame frame = userspace_return_frame();

	cr_assert(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	userspace_return_set_entry(&frame, TEST_USER_ENTRY + 1u);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	frame = userspace_return_frame();
	userspace_return_set_stack(&frame, 0u);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	frame = userspace_return_frame();
	userspace_return_set_stack(&frame, TEST_USER_STACK + 1u);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	frame = userspace_return_frame();
	userspace_return_set_privileged(&frame);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	frame = userspace_return_frame();
	userspace_return_set_unsafe_flags(&frame);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
	frame = userspace_return_frame();
	userspace_return_set_entry(&frame, 0x1000u + MM_USER_ADDRESS_SPACE_SIZE);
	cr_assert_not(hal_userspace_frame_valid((const struct hal_userspace_return_frame*)&frame));
}
