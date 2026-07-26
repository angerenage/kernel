#include <core/user_return.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <stddef.h>

void core_finalize_user_return(struct hal_userspace_return_frame* frame) {
	struct uthread*         current;
	enum user_upcall_result result;

	if (frame == NULL || !hal_userspace_frame_is_user(frame)) return;

	current = uthread_current();
	if (current == NULL || current->process == NULL) hcf();

	result = uthread_upcall_deliver(current, frame);
	if (result != USER_UPCALL_OK && result != USER_UPCALL_IDLE) hcf();
}
