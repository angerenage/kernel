#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Raw syscall entry point. Implemented per architecture in userspace/platforms/<platform>/syscall.c. */
syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5);
