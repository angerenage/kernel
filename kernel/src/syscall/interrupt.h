#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Bind or detach a Signal capability from one platform interrupt source. */
syscall_result_t syscall_interrupt_attach(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_interrupt_detach(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
