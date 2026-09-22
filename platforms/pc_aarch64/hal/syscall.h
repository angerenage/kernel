#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "interrupts/frame.h"

/* Dispatch an AArch64 synchronous exception when it is a userspace syscall. */
bool aarch64_handle_syscall(struct exception_frame* frame, uint64_t ec);
