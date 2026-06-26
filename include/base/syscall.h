#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
	SYSCALL_NOP = 0u,

	/* Scheduler / time */
	SYSCALL_YIELD,
	SYSCALL_SLEEP_MS,
	SYSCALL_TICK_COUNT,

	/* Process identity */
	SYSCALL_SELF,

	/* Process lifecycle */
	SYSCALL_EXIT_PROCESS,
	SYSCALL_CREATE_PROCESS,

	/* Thread lifecycle */
	SYSCALL_EXIT_THREAD,
	SYSCALL_SPAWN_THREAD,
	SYSCALL_JOIN_THREAD,
	SYSCALL_DETACH_THREAD,
	SYSCALL_CANCEL_THREAD,
	SYSCALL_SET_THREAD_CANCEL_ENABLED,
	SYSCALL_TEST_THREAD_CANCEL,

	/* Message passing */
	SYSCALL_SEND_MESSAGE,
	SYSCALL_RECV_MESSAGE,

	/* Channels */
	SYSCALL_CHANNEL_CREATE,
	SYSCALL_CHANNEL_DESTROY,

	/* Boot module */
	SYSCALL_MODULE_RESOLVE,

	/* Capability system */
	SYSCALL_CAP_CREATE,
	SYSCALL_CAP_DELEGATE,
	SYSCALL_CAP_DERIVE,
	SYSCALL_CAP_CALL,
	SYSCALL_CAP_REVOKE,
	SYSCALL_CAP_RECV,

	SYSCALL_COUNT,
};

typedef enum syscall_status {
	SYSCALL_STATUS_OK              = 0u,
	SYSCALL_STATUS_UNKNOWN_SYSCALL = 100u,
	SYSCALL_STATUS_BAD_ARGUMENT    = 101u,
	SYSCALL_STATUS_DENIED          = 102u,
	SYSCALL_STATUS_FAILED          = 200u,
	SYSCALL_STATUS_UNAVAILABLE     = 201u,
} syscall_status_t;

/* Result envelope returned by every syscall implementation. */
typedef struct syscall_result {
	uintptr_t        value;
	syscall_status_t status;
} syscall_result_t;

/* Convert an arbitrary value into a successful syscall_result_t. */
static inline syscall_result_t syscall_result_ok(uintptr_t value) {
	return (syscall_result_t){
		.value  = value,
		.status = SYSCALL_STATUS_OK,
	};
}

/* Convert an arbitrary value and error status into a failing syscall_result_t. */
static inline syscall_result_t syscall_result_error(syscall_status_t status, uintptr_t value) {
	return (syscall_result_t){
		.value  = value,
		.status = status,
	};
}

/* Return true when the status indicates a successful syscall completion. */
static inline bool syscall_status_is_success(syscall_status_t status) {
	return status == SYSCALL_STATUS_OK;
}

/* Return true when the status indicates a malformed or unsupported caller request. */
static inline bool syscall_status_is_caller_error(syscall_status_t status) {
	return status >= 100u && status < 200u;
}

/* Return true when the status indicates a kernel-side failure during a valid request. */
static inline bool syscall_status_is_kernel_error(syscall_status_t status) {
	return status >= 200u && status < 300u;
}

/* Dispatch a syscall number to the registered handler. Implemented by the environment linking base. */
syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5);
