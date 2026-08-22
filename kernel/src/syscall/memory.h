#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Create a logical memory object and return its capability. */
syscall_result_t syscall_memory_create(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
