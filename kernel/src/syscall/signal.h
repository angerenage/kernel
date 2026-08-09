#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Create a Signal and return a capability granting full synchronous rights to the caller. */
syscall_result_t syscall_signal_create(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Fast paths for Signal publication and synchronous reception. */
syscall_result_t syscall_signal_send(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5);
syscall_result_t syscall_signal_send_coalesced(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                               uintptr_t arg4, uintptr_t arg5);
syscall_result_t syscall_signal_read(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_signal_wait(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_signal_try_wait(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
