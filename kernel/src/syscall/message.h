#pragma once

#include <base/syscall.h>

syscall_result_t syscall_send_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_recv_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
