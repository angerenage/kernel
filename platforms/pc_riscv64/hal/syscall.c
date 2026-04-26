#include <core/syscall.h>
#include <stdint.h>

#include "interrupts_private.h"

bool riscv64_handle_syscall(struct exception_frame* frame, bool is_interrupt, uint64_t code) {
	if (is_interrupt) return false;
	if (code != 8u) return false;

	frame->a0 = syscall_dispatch(frame->a7, frame->a0, frame->a1, frame->a2, frame->a3, frame->a4, frame->a5);
	frame->sepc += 4u;
	return true;
}
