#include <core/user_return.h>
#include <core/uthread.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <stddef.h>

void core_finalize_user_return(struct hal_userspace_return_frame* frame) {
	struct uthread* current;

	if (frame == NULL || !hal_userspace_frame_is_user(frame)) return;

	current = uthread_current();
	if (current == NULL || current->process == NULL) hcf();
}
