#pragma once

#include <stdbool.h>
#include <stdint.h>

#define THREAD_EXIT_CODE_CANCELLED ((uintptr_t)UINTPTR_MAX)
#define THREAD_START_ARG_MAX_SIZE 4096u

/* Operation codes for thread capability requests. */
enum thread_op {
	THREAD_OP_JOIN = 0,
	THREAD_OP_DETACH,
	THREAD_OP_CANCEL,
	THREAD_OP_SET_CANCEL_ENABLED,
	THREAD_OP_TEST_CANCEL,
};

/* Common header for all thread capability requests. */
struct thread_request_header {
	enum thread_op op;
};

/* Request to wait for a thread to terminate. */
struct thread_join_request {
	struct thread_request_header header;
};

/* Response with a thread's exit code. */
struct thread_join_response {
	uintptr_t exit_code;
};

/* Request to detach a thread. */
struct thread_detach_request {
	struct thread_request_header header;
};

/* Request deferred cancellation of a thread. */
struct thread_cancel_request {
	struct thread_request_header header;
};

/* Enable or disable deferred cancellation on the target thread. */
struct thread_set_cancel_enabled_request {
	struct thread_request_header header;
	bool                         enabled;
};

/* Test deferred cancellation on the target thread. */
struct thread_test_cancel_request {
	struct thread_request_header header;
};

/* Response indicating whether the target thread has actionable pending cancellation. */
struct thread_test_cancel_response {
	bool should_cancel;
};
