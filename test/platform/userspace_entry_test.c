#include <stddef.h>
#include <stdint.h>

#include <hal/userspace.h>

#include <criterion/criterion.h>

#define TEST_KERNEL_STACK_TOP 0x10000u
#define TEST_USER_ENTRY 0x4000u
#define TEST_USER_STACK 0x8000u

#if defined(__aarch64__)
void aarch64_userspace_enter(void) {
}
#elif defined(__riscv)
void riscv64_userspace_enter(void) {
}
#elif defined(__loongarch64)
void loongarch64_userspace_enter(void) {
}
#endif

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
