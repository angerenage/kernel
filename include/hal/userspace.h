#pragma once

#include <hal/cpu.h>
#include <stdbool.h>
#include <stdint.h>

/* Architecture hook for entry into userspace. */

/*
 * Initialize a thread frame so the first context switch into it
 * enters user_entry(user_arg) on the supplied user stack.
 */
bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg);
