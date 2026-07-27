#include <core/syscall.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t marker;
};

static struct uthread          fake_thread;
static struct uthread*         current_thread;
static enum user_upcall_result restore_result;
static size_t                  dispatch_count;

/* Reset the syscall test state. */
static void upcall_syscall_test_reset(void) {
	memset(&fake_thread, 0, sizeof(fake_thread));
	fake_thread.process = (struct process*)(uintptr_t)1u;
	current_thread      = &fake_thread;
	restore_result      = USER_UPCALL_OK;
	dispatch_count      = 0u;
}

/* Return the fake current thread. */
struct uthread* uthread_current(void) {
	return current_thread;
}

/* Restore the fake interrupted frame. */
enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	cr_assert_eq(thread, current_thread);
	if (restore_result == USER_UPCALL_OK) frame->marker = 0xdecafbadull;
	return restore_result;
}

/* Return whether the fake frame is a userspace frame. */
bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return frame != NULL && frame->user;
}

/* Record a normal syscall dispatch. */
syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	(void)number;
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;
	dispatch_count++;
	return syscall_result_error(SYSCALL_STATUS_FAILED, 0x1234u);
}

Test(upcall_syscall, normal_syscalls_use_the_generic_dispatcher) {
	struct hal_userspace_return_frame frame  = {.user = true};
	syscall_result_t                  result = {0};

	upcall_syscall_test_reset();
	cr_assert_eq(syscall_dispatch_user_frame(&frame, 7u, 1u, 2u, 3u, 4u, 5u, 6u, &result), SYSCALL_FRAME_WRITE_RESULT);
	cr_assert_eq(dispatch_count, 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(result.value, 0x1234u);
}

Test(upcall_syscall, successful_return_restores_the_native_frame) {
	struct hal_userspace_return_frame frame  = {.user = true, .marker = 0x55u};
	syscall_result_t                  result = syscall_result_error(SYSCALL_STATUS_FAILED, 0x99u);

	upcall_syscall_test_reset();
	cr_assert_eq(syscall_dispatch_user_frame(&frame, SYSCALL_UPCALL_RETURN, 0u, 0u, 0u, 0u, 0u, 0u, &result),
	             SYSCALL_FRAME_RESTORED);
	cr_assert_eq(frame.marker, 0xdecafbadull);
	cr_assert_eq(dispatch_count, 0u);
	cr_assert_eq(result.value, 0x99u);
}

Test(upcall_syscall, return_outside_an_upcall_is_denied) {
	struct hal_userspace_return_frame frame  = {.user = true};
	syscall_result_t                  result = {0};

	upcall_syscall_test_reset();
	restore_result = USER_UPCALL_NOT_ACTIVE;
	cr_assert_eq(syscall_dispatch_user_frame(&frame, SYSCALL_UPCALL_RETURN, 0u, 0u, 0u, 0u, 0u, 0u, &result),
	             SYSCALL_FRAME_WRITE_RESULT);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
}
