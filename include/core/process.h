#pragma once

#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t process_id_t;

#define PROCESS_PID_INVALID ((process_id_t)0u)

struct cpu;
struct uthread;

enum process_state {
	PROCESS_STATE_NEW = 0,
	PROCESS_STATE_RUNNING,
	PROCESS_STATE_EXITING,
	PROCESS_STATE_ZOMBIE,
};

enum process_result {
	PROCESS_OK = 0,
	PROCESS_INVALID_ARGUMENTS,
	PROCESS_NO_MEMORY,
	PROCESS_ADDRESS_SPACE_FAILED,
	PROCESS_PID_EXHAUSTED,
	PROCESS_THREAD_STACK_ALLOC_FAILED,
	PROCESS_THREAD_CONTEXT_UNSUPPORTED,
	PROCESS_THREAD_SCHEDULER_REJECTED,
	PROCESS_THREAD_REAPER_UNAVAILABLE,
};

struct process_spawn_params {
	const char* name;
	uintptr_t   user_entry;
	uintptr_t   user_arg;
	size_t      user_stack_pages;
	struct cpu* preferred_cpu;
};

struct process_thread_params {
	const char* name;
	uintptr_t   user_entry;
	uintptr_t   user_arg;
	size_t      user_stack_pages;
	struct cpu* preferred_cpu;
	bool        detached;
};

struct process {
	process_id_t         pid;
	const char*          name;
	enum process_state   state;
	uintptr_t            exit_code;
	size_t               thread_count;
	struct process*      parent;
	struct uthread*      main_thread;
	struct uthread*      thread_head;
	struct uthread*      thread_tail;
	struct address_space address_space;
	struct spinlock      lock;
};

/* Create a process and start its main userspace thread. */
enum process_result process_spawn(struct process** out_process, const struct process_spawn_params* params);

/* Initialize caller-owned userspace thread storage inside process and queue it on the scheduler. */
enum process_result process_create_thread(struct process* process, struct uthread* thread,
                                          const struct process_thread_params* params);

/* Destroy all reclaimable process threads, release its user address space, and free the process. */
bool process_destroy(struct process* process);

/* Return a process PID, or PROCESS_PID_INVALID for NULL. */
process_id_t process_pid(const struct process* process);

/* Return the mutable user address space owned by process. */
struct address_space* process_address_space(struct process* process);

/* Return the main userspace thread for process, or NULL. */
struct uthread* process_main_thread(struct process* process);

/* Return the current process state. */
enum process_state process_get_state(struct process* process);

/* Return the number of threads currently attached to process. */
size_t process_thread_count(struct process* process);
