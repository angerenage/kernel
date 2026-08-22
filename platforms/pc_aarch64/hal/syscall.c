#include <kernel/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

bool aarch64_handle_syscall(struct exception_frame* frame, uint64_t ec) {
	uint64_t                  vector = frame->vector & 0xfu;
	enum syscall_frame_action action;
	syscall_result_t          result;

	if (vector != 8u) return false;
	if (ec != 0x15u) return false;

	action = syscall_dispatch_user_frame((struct hal_userspace_return_frame*)frame,
	                                     frame->x[8],
	                                     frame->x[0],
	                                     frame->x[1],
	                                     frame->x[2],
	                                     frame->x[3],
	                                     frame->x[4],
	                                     frame->x[5],
	                                     &result);
	if (action == SYSCALL_FRAME_RESTORED) return true;
	frame->x[0] = result.value;
	frame->x[1] = (uint64_t)result.status;
	return true;
}
