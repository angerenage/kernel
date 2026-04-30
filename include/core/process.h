#pragma once

#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t process_id_t;

#define PROCESS_PID_INVALID ((process_id_t)0u)

enum process_state {
	PROCESS_STATE_NEW = 0,
	PROCESS_STATE_RUNNING,
	PROCESS_STATE_EXITING,
	PROCESS_STATE_ZOMBIE,
};

enum process_create_result {
	PROCESS_CREATE_OK = 0,
	PROCESS_CREATE_INVALID_ARGUMENTS,
	PROCESS_CREATE_NO_MEMORY,
	PROCESS_CREATE_ADDRESS_SPACE_FAILED,
	PROCESS_CREATE_PID_EXHAUSTED,
};

struct process {
	process_id_t         pid;
	const char*          name;
	enum process_state   state;
	uintptr_t            exit_code;
	size_t               thread_count;
	struct process*      parent;
	struct address_space address_space;
	struct spinlock      lock;
};

/* Allocate a process descriptor and initialize its user address space. */
enum process_create_result process_create(struct process** out_process, const char* name);

/* Destroy a process with no attached threads and release its user address space. */
bool process_destroy(struct process* process);

/* Return the current process state. */
enum process_state process_get_state(struct process* process);

/* Return the number of threads currently attached to process. */
size_t process_thread_count(struct process* process);
