#include <base/process.h>
#include <core/process.h>
#include <core/user_return.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <stddef.h>

static void core_handle_upcall_delivery_result(struct uthread* current, enum user_upcall_result result) {
	switch (result) {
	case USER_UPCALL_OK:
	case USER_UPCALL_IDLE:
	case USER_UPCALL_DEFERRED:
	case USER_UPCALL_THREAD_DYING:
		return;
	case USER_UPCALL_CONTEXT_INVALID:
		(void)process_terminate(current->process, PROCESS_EXIT_SYSTEM_UPCALL_CONTEXT_INVALID);
		return;
	case USER_UPCALL_INVALID_ARGUMENTS:
	case USER_UPCALL_QUEUE_FULL:
	case USER_UPCALL_NOT_ACTIVE:
	default:
		hcf();
	}
}

void core_finalize_user_return(struct hal_userspace_return_frame* frame) {
	struct uthread*         current;
	enum user_upcall_result result;

	if (frame == NULL || !hal_userspace_frame_is_user(frame)) return;

	current = uthread_current();
	if (current == NULL || current->process == NULL) hcf();

	result = uthread_upcall_deliver(current, frame);
	core_handle_upcall_delivery_result(current, result);
}
