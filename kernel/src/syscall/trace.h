#pragma once

#include <base/syscall.h>

syscall_result_t syscall_trace_register(uintptr_t name, uintptr_t name_length, uintptr_t, uintptr_t, uintptr_t,
                                        uintptr_t);
syscall_result_t syscall_trace_event(uintptr_t type, uintptr_t label, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
