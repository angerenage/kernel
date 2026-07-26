#include <core/user_return.h>
#include <core/user_upcall.h>
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

static size_t                  frame_checks;
static size_t                  current_checks;
static size_t                  delivery_checks;
static enum user_upcall_result delivery_result;
static struct uthread          fake_current;
static struct uthread*         current_thread;

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	frame_checks++;
	return frame != NULL && frame->user;
}

struct uthread* uthread_current(void) {
	current_checks++;
	return current_thread;
}

enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	delivery_checks++;
	cr_assert_eq(thread, current_thread);
	cr_assert_not_null(frame);
	return delivery_result;
}

__attribute__((noreturn))
void hcf(void) {
	abort();
}

static void user_return_test_reset(void) {
	frame_checks         = 0u;
	current_checks       = 0u;
	delivery_checks      = 0u;
	delivery_result      = USER_UPCALL_IDLE;
	fake_current         = (struct uthread){0};
	fake_current.process = (struct process*)(uintptr_t)1u;
	current_thread       = &fake_current;
}

Test(user_return, ignores_null_frame) {
	user_return_test_reset();

	core_finalize_user_return(NULL);

	cr_assert_eq(frame_checks, 0u);
	cr_assert_eq(current_checks, 0u);
	cr_assert_eq(delivery_checks, 0u);
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
	cr_assert_eq(delivery_checks, 0u);
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
	cr_assert_eq(delivery_checks, 1u);
	cr_assert_eq(frame.marker, 0x8877665544332211ull);
}

Test(user_return, accepts_delivered_upcall) {
	struct hal_userspace_return_frame frame = {
		.user   = true,
		.marker = 0x123456789abcdef0ull,
	};

	user_return_test_reset();
	delivery_result = USER_UPCALL_OK;
	core_finalize_user_return(&frame);

	cr_assert_eq(frame_checks, 1u);
	cr_assert_eq(current_checks, 1u);
	cr_assert_eq(delivery_checks, 1u);
}
