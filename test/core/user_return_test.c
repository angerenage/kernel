#include <core/user_return.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t marker;
};

static size_t          frame_checks;
static size_t          current_checks;
static struct uthread  fake_current;
static struct uthread* current_thread;

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	frame_checks++;
	return frame != NULL && frame->user;
}

struct uthread* uthread_current(void) {
	current_checks++;
	return current_thread;
}

__attribute__((noreturn))
void hcf(void) {
	abort();
}

static void user_return_test_reset(void) {
	frame_checks         = 0u;
	current_checks       = 0u;
	fake_current         = (struct uthread){0};
	fake_current.process = (struct process*)(uintptr_t)1u;
	current_thread       = &fake_current;
}

Test(user_return, ignores_null_frame) {
	user_return_test_reset();

	core_finalize_user_return(NULL);

	cr_assert_eq(frame_checks, 0u);
	cr_assert_eq(current_checks, 0u);
}

Test(user_return, ignores_kernel_return) {
	struct hal_userspace_return_frame frame = {
		.user   = false,
		.marker = 0x1122334455667788ull,
	};

	user_return_test_reset();
	core_finalize_user_return(&frame);

	cr_assert_eq(frame_checks, 1u);
	cr_assert_eq(current_checks, 0u);
	cr_assert_eq(frame.marker, 0x1122334455667788ull);
}

Test(user_return, validates_userspace_owner_without_mutating_frame) {
	struct hal_userspace_return_frame frame = {
		.user   = true,
		.marker = 0x8877665544332211ull,
	};

	user_return_test_reset();
	core_finalize_user_return(&frame);

	cr_assert_eq(frame_checks, 1u);
	cr_assert_eq(current_checks, 1u);
	cr_assert_eq(frame.marker, 0x8877665544332211ull);
}
