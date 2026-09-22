#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "interrupts/frame.h"

/* Dispatch a LoongArch exception when it is a userspace syscall. */
bool loongarch64_handle_syscall(struct exception_frame* frame, uint64_t ecode);
