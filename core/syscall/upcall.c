#include <base/process.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <hal/userspace.h>
#include <stddef.h>
#include <stdint.h>

static syscall_result_t syscall_upcall_result(enum user_upcall_result result) {
	switch (result) {
	case USER_UPCALL_NOT_ACTIVE:
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	case USER_UPCALL_THREAD_DYING:
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	case USER_UPCALL_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case USER_UPCALL_CONTEXT_INVALID:
	case USER_UPCALL_QUEUE_FULL:
	case USER_UPCALL_DEFERRED:
	case USER_UPCALL_IDLE:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
}

enum syscall_frame_action syscall_dispatch_user_frame(struct hal_userspace_return_frame* frame, uintptr_t number,
                                                      uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                                      uintptr_t arg4, uintptr_t arg5, syscall_result_t* out_result) {
	struct uthread*         current;
	enum user_upcall_result restore_result;

	if (out_result == NULL) return SYSCALL_FRAME_WRITE_RESULT;
	if (number != SYSCALL_UPCALL_RETURN) {
		*out_result = syscall_dispatch(number, arg0, arg1, arg2, arg3, arg4, arg5);
		return SYSCALL_FRAME_WRITE_RESULT;
	}
	if (frame == NULL || !hal_userspace_frame_is_user(frame)) {
		*out_result = syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return SYSCALL_FRAME_WRITE_RESULT;
	}

	current = uthread_current();
	if (current == NULL || current->process == NULL) {
		*out_result = syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		return SYSCALL_FRAME_WRITE_RESULT;
	}

	restore_result = uthread_upcall_restore(current, frame);
	if (restore_result == USER_UPCALL_OK) return SYSCALL_FRAME_RESTORED;
	if (restore_result == USER_UPCALL_CONTEXT_INVALID) {
		*out_result = syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		(void)process_terminate(current->process, PROCESS_EXIT_SYSTEM_UPCALL_CONTEXT_INVALID);
		return SYSCALL_FRAME_WRITE_RESULT;
	}
	*out_result = syscall_upcall_result(restore_result);
	return SYSCALL_FRAME_WRITE_RESULT;
}
