#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "interrupts/frame.h"

/* Dispatch a RISC-V supervisor trap when it is a userspace syscall. */
bool riscv64_handle_syscall(struct exception_frame* frame, bool is_interrupt, uint64_t code);
