#pragma once

#include <stdbool.h>

#include "interrupts/frame.h"

/* Enable the x86-64 fast syscall entry path on the current processor. */
void x86_64_syscall_init(void);

/* Dispatch an interrupt frame when it contains a userspace syscall. */
bool x86_64_handle_syscall(struct interrupt_frame* frame);

/* Select a safe userspace return mechanism for a restored frame. */
enum x86_user_return_kind x86_64_classify_user_return(const struct user_interrupt_frame* frame);

/* Terminate the current process after detecting an invalid userspace return frame. */
__attribute__((noreturn))
void x86_64_reject_user_return(void);
