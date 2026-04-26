#include <core/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

bool aarch64_handle_syscall(struct exception_frame* frame, uint64_t ec) {
	uint64_t vector = frame->vector & 0xfu;

	if (vector != 8u) return false;
	if (ec != 0x15u) return false;

	frame->x[0] =
		syscall_dispatch(frame->x[8], frame->x[0], frame->x[1], frame->x[2], frame->x[3], frame->x[4], frame->x[5]);
	return true;
}
