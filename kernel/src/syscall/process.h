#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Return a struct self_info describing the calling process and a capability to it. */
syscall_result_t syscall_self(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Create a new process and return a capability that grants full process-management rights to the caller. */
syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Terminate the calling process with the given exit code. */
syscall_result_t syscall_exit_process(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
