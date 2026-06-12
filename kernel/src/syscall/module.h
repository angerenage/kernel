#pragma once

#include <base/syscall.h>
#include <stdint.h>

syscall_result_t syscall_module_resolve(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5);
