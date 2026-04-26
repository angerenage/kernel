#pragma once

#include <stdint.h>

enum {
	SYSCALL_NOP = 0u,
	SYSCALL_YIELD,
	SYSCALL_SLEEP_MS,
};

typedef uintptr_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5);

uintptr_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                           uintptr_t arg4, uintptr_t arg5);
