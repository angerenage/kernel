#pragma once

#include <base/cap.h>
#include <base/thread.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t process_id_t;

#define PROCESS_PID_INVALID ((process_id_t)0u)

/* Machine-word-sized exit code published by a process at termination. */
typedef uintptr_t process_exit_code_t;

/* Process exit codes grouped by family: each nibble reserves a class of faults/errors. */
enum process_exit_code {
	PROCESS_EXIT_SUCCESS = 0u,

	PROCESS_EXIT_SYSTEM_UNKNOWN = 0x100u,
	PROCESS_EXIT_SYSTEM_INVALID_STARTUP,
	PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED,
	PROCESS_EXIT_SYSTEM_UPCALL_CONTEXT_INVALID,

	/* Memory access faults. */
	PROCESS_EXIT_MEMORY_NOT_PRESENT = 0x200u,
	PROCESS_EXIT_MEMORY_PROTECTION,
	PROCESS_EXIT_MEMORY_INVALID,

	/* Arithmetic exceptions. */
	PROCESS_EXIT_ARITHMETIC_DIVIDE_BY_ZERO = 0x300u,
	PROCESS_EXIT_ARITHMETIC_OVERFLOW,
	PROCESS_EXIT_ARITHMETIC_BOUND_RANGE,

	/* Illegal or undefined instruction. */
	PROCESS_EXIT_INSTRUCTION_ILLEGAL = 0x400u,

	/* General privilege or protection violation. */
	PROCESS_EXIT_PRIVILEGE_GENERAL_PROTECTION = 0x500u,

	/* Data alignment fault. */
	PROCESS_EXIT_ALIGNMENT_FAULT = 0x600u,

	/* Access aborts (instruction/data) and address errors. */
	PROCESS_EXIT_ACCESS_INSTRUCTION_ABORT = 0x700u,
	PROCESS_EXIT_ACCESS_DATA_ABORT,
	PROCESS_EXIT_ACCESS_ADDRESS_ERROR_FETCH,
	PROCESS_EXIT_ACCESS_ADDRESS_ERROR_MEMORY,

	/* Bus error. */
	PROCESS_EXIT_BUS_ERROR = 0x800u,

	/* Floating-point and SIMD exceptions. */
	PROCESS_EXIT_FLOATING_POINT_ERROR = 0x900u,
	PROCESS_EXIT_FLOATING_POINT_SIMD,
};

/* Identity information returned by the self syscall. */
struct self_info {
	process_id_t pid;
	uint64_t     thread_id;
	uint64_t     thread_count;
	/* Capability for the calling process. */
	cap_id_t self_cap;
	/* Capability for the calling process' address space. */
	cap_id_t address_space_cap;
	/* Capability for the calling process' main thread. */
	cap_id_t main_thread_cap;
};

/* Capabilities returned to the creator of a new process. */
struct process_create_response {
	cap_id_t process_cap;
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

/* Response with the ID and capability of a process' newly-started main thread. */
struct process_run_response {
	uint64_t thread_id;
	cap_id_t thread_cap;
};

/* Response with a capability for a newly-created thread. */
struct process_spawn_thread_response {
	cap_id_t thread_cap;
};

/* Operation codes for process capability requests. */
enum process_op {
	PROCESS_OP_INFO         = 0,
	PROCESS_OP_RUN          = 1,
	PROCESS_OP_WAIT         = 2,
	PROCESS_OP_DETACH       = 3,
	PROCESS_OP_KILL         = 4,
	PROCESS_OP_SPAWN_THREAD = 5,
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

/* Request to run a process. arg_data is copied from the caller onto the new thread's stack. */
struct process_run_request {
	struct process_request_header header;
	uintptr_t                     entry;
	const void*                   arg_data;
	size_t                        arg_size;
};

/* Request to create a joinable thread. arg_data is copied from the caller onto the new thread's stack. */
struct process_spawn_thread_request {
	struct process_request_header header;
	uintptr_t                     entry;
	const void*                   arg_data;
	size_t                        arg_size;
	const char*                   name;
	size_t                        name_length;
};
