#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
	SYSCALL_NOP = 0u,

	/* Scheduler / time */
	SYSCALL_YIELD,
	SYSCALL_SLEEP_MS,
	SYSCALL_TICK_COUNT,

	/* Temporary diagnostics */
	SYSCALL_PRINT,

	/* Process identity / introspection */
	SYSCALL_GETPID,
	SYSCALL_GET_PROCESS_THREAD_COUNT,

	/* Thread identity / introspection */
	SYSCALL_GETTID,

	/* Process lifecycle */
	SYSCALL_EXIT_PROCESS,
	SYSCALL_CREATE_PROCESS,
	SYSCALL_RUN_PROCESS,
	SYSCALL_WAIT_PROCESS,
	SYSCALL_DETACH_PROCESS,
	SYSCALL_KILL_PROCESS,

	/* Thread lifecycle */
	SYSCALL_EXIT_THREAD,
	SYSCALL_SPAWN_THREAD,
	SYSCALL_JOIN_THREAD,
	SYSCALL_DETACH_THREAD,
	SYSCALL_CANCEL_THREAD,
	SYSCALL_SET_THREAD_CANCEL_ENABLED,
	SYSCALL_TEST_THREAD_CANCEL,

	/* Virtual memory */
	SYSCALL_VM_RESERVE,
	SYSCALL_VM_RESERVE_AT,
	SYSCALL_VM_FREE,
	SYSCALL_VM_MAP,
	SYSCALL_VM_UNMAP,
	SYSCALL_VM_PROTECT,
	SYSCALL_VM_QUERY,
	SYSCALL_VM_COPY_FROM,
	SYSCALL_VM_COPY_TO,

	/* Message passing */
	SYSCALL_SEND_MESSAGE,
	SYSCALL_RECV_MESSAGE,

	/* Channels */
	SYSCALL_CHANNEL_CREATE,
	SYSCALL_CHANNEL_SEND,
	SYSCALL_CHANNEL_RECV,
	SYSCALL_CHANNEL_DESTROY,

	/* Boot modules - temporary solution for loading additional ELF files */
	SYSCALL_MODULE_RESOLVE,
	SYSCALL_MODULE_MAP,

	SYSCALL_COUNT,
};

typedef enum syscall_status {
	SYSCALL_STATUS_OK              = 0u,
	SYSCALL_STATUS_UNKNOWN_SYSCALL = 100u,
	SYSCALL_STATUS_BAD_ARGUMENT    = 101u,
	SYSCALL_STATUS_FAILED          = 200u,
	SYSCALL_STATUS_UNAVAILABLE     = 201u,
} syscall_status_t;

typedef struct syscall_result {
	uintptr_t        value;
	syscall_status_t status;
} syscall_result_t;

static inline syscall_result_t syscall_result_ok(uintptr_t value) {
	return (syscall_result_t){
		.value  = value,
		.status = SYSCALL_STATUS_OK,
	};
}

static inline syscall_result_t syscall_result_error(syscall_status_t status, uintptr_t value) {
	return (syscall_result_t){
		.value  = value,
		.status = status,
	};
}

static inline bool syscall_status_is_success(syscall_status_t status) {
	return status == SYSCALL_STATUS_OK;
}

static inline bool syscall_status_is_caller_error(syscall_status_t status) {
	return status >= 100u && status < 200u;
}

static inline bool syscall_status_is_kernel_error(syscall_status_t status) {
	return status >= 200u && status < 300u;
}
