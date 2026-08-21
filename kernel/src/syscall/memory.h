#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Allocate a new memory region and return a capability to it. */
syscall_result_t syscall_memory_allocate(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
