#include <stddef.h>
#include <stdint.h>

#include <hal/userspace.h>

#include <criterion/criterion.h>

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

Test(userspace_entry, rejects_null_and_misaligned_entries) {
	cr_assert(hal_userspace_entry_valid(0x4000u));
	cr_assert_not(hal_userspace_entry_valid(0u));
	cr_assert_not(hal_userspace_entry_valid(0x4001u));
}
