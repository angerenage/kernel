#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
	SYSCALL_NOP = 0u,
	SYSCALL_YIELD,
	SYSCALL_SLEEP_MS,
	SYSCALL_COUNT,
};

typedef enum syscall_status {
	SYSCALL_STATUS_OK              = 0u,
	SYSCALL_STATUS_UNKNOWN_SYSCALL = 100u,
	SYSCALL_STATUS_BAD_ARGUMENT    = 101u,
	SYSCALL_STATUS_FAILED          = 200u,
	SYSCALL_STATUS_UNAVAILABLE     = 201u,
} syscall_status_t;

typedef syscall_status_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

static inline bool syscall_status_is_success(syscall_status_t status) {
	return status == SYSCALL_STATUS_OK;
}

static inline bool syscall_status_is_caller_error(syscall_status_t status) {
	return status >= 100u && status < 200u;
}

static inline bool syscall_status_is_kernel_error(syscall_status_t status) {
	return status >= 200u && status < 300u;
}

syscall_status_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5);
