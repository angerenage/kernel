#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdint.h>

/* Identity information returned by the self syscall. */
struct self_info {
	process_id_t pid;
	uint64_t     thread_id;
	uint64_t     thread_count;
	/* Capability for the calling process. */
	cap_id_t self_cap;
	/* Capability for the calling process' address space. */
	cap_id_t address_space_cap;
};

/* Response with process identity information. */
struct process_info_response {
	process_id_t pid;
	uint64_t     thread_id;
	uint64_t     thread_count;
};

/* Response with process exit code. */
struct process_wait_response {
	uintptr_t exit_code;
};

/* Operation codes for process capability requests. */
enum process_op {
	PROCESS_OP_INFO   = 0,
	PROCESS_OP_RUN    = 1,
	PROCESS_OP_WAIT   = 2,
	PROCESS_OP_DETACH = 3,
	PROCESS_OP_KILL   = 4,
};

/* Common header for all process capability requests. */
struct process_request_header {
	enum process_op op;
};

/* Request to get process identity information. */
struct process_info_request {
	struct process_request_header header;
};

/* Request to wait for process termination. */
struct process_wait_request {
	struct process_request_header header;
};

/* Request to detach a process. */
struct process_detach_request {
	struct process_request_header header;
};

/* Request to kill a process. */
struct process_kill_request {
	struct process_request_header header;
	uintptr_t                     exit_code;
};

/* Request to run a process. */
struct process_run_request {
	struct process_request_header header;
	uintptr_t                     entry;
	uintptr_t                     arg;
	size_t                        stack_pages;
};
