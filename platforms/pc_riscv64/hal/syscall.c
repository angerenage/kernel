#include <core/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

bool riscv64_handle_syscall(struct exception_frame* frame, bool is_interrupt, uint64_t code) {
	enum syscall_frame_action action;
	syscall_result_t          result;

	if (is_interrupt) return false;
	if (code != 8u) return false;

	action = syscall_dispatch_user_frame((struct hal_userspace_return_frame*)frame,
	                                     frame->a7,
	                                     frame->a0,
	                                     frame->a1,
	                                     frame->a2,
	                                     frame->a3,
	                                     frame->a4,
	                                     frame->a5,
	                                     &result);
	if (action == SYSCALL_FRAME_RESTORED) return true;
	frame->a0 = result.value;
	frame->a1 = (uint64_t)result.status;
	frame->sepc += 4u;
	return true;
}
