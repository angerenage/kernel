#pragma once

#include <base/syscall.h>

/* Create a channel and optionally return a capability for its activity signal. */
syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Destroy a channel owned by the calling process. */
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Receive one pending lifecycle event without blocking. */
syscall_result_t syscall_channel_event_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
