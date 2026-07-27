#include <core/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

bool loongarch64_handle_syscall(struct exception_frame* frame, uint64_t ecode) {
	enum syscall_frame_action action;
	syscall_result_t          result;

	if (ecode != 0xbu) return false;

	action = syscall_dispatch_user_frame((struct hal_userspace_return_frame*)frame,
	                                     frame->gpr[11],
	                                     frame->gpr[4],
	                                     frame->gpr[5],
	                                     frame->gpr[6],
	                                     frame->gpr[7],
	                                     frame->gpr[8],
	                                     frame->gpr[9],
	                                     &result);
	if (action == SYSCALL_FRAME_RESTORED) return true;
	frame->gpr[4] = result.value;
	frame->gpr[5] = (uint64_t)result.status;
	frame->era += 4u;
	return true;
}
