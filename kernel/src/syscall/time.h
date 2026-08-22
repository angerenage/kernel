#pragma once

#include <base/syscall.h>

syscall_result_t syscall_yield(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_tick_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
