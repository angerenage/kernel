#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Common entry-point shape implemented by every syscall handler. */
typedef syscall_result_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

/* Dispatch a syscall number to the registered handler, returning SYSCALL_STATUS_UNKNOWN_SYSCALL when absent. */
syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5);
