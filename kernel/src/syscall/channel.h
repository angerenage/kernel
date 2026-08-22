#pragma once

#include <base/syscall.h>

syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_channel_event_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
