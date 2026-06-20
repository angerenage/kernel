#pragma once

#include <base/syscall.h>
#include <stddef.h>

/* Return a struct self_info describing the calling process and a capability to it. */
syscall_result_t syscall_self(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
