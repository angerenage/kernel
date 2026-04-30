#pragma once

#include <core/thread.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	UTHREAD_DEFAULT_USER_STACK_PAGES = 4u,
};

enum uthread_start_result {
	UTHREAD_START_OK = 0,
	UTHREAD_START_INVALID_ARGUMENTS,
	UTHREAD_START_NO_MEMORY,
	UTHREAD_START_STACK_ALLOC_FAILED,
	UTHREAD_START_CONTEXT_UNSUPPORTED,
	UTHREAD_START_SCHEDULER_REJECTED,
	UTHREAD_START_REAPER_UNAVAILABLE,
};

struct uthread_start_params {
	const char*           name;
	struct address_space* address_space;
	uintptr_t             user_entry;
	uintptr_t             user_arg;
	size_t                user_stack_pages;
	struct cpu*           preferred_cpu;
	bool                  detached;
};

struct uthread {
	struct thread         thread;
	struct address_space* address_space;
	vmm_id_t              user_stack_id;
	vmm_id_t              kernel_stack_id;
	uintptr_t             user_stack_top;
	struct uthread*       reaper_next;
	bool                  heap_allocated;
};

/* Initialize caller-owned userspace thread storage and queue it on the scheduler. */
enum uthread_start_result uthread_start(struct uthread* thread, const struct uthread_start_params* params);

/*
 * Allocate, initialize, and queue a detached userspace thread.
 * The user address space remains caller-owned; the thread descriptor and stacks
 * are reclaimed by the user-thread reaper after exit.
 */
enum uthread_start_result uthread_spawn_detached(const struct uthread_start_params* params);

/* Convert a live joinable userspace thread into a detached thread. */
bool uthread_detach(struct uthread* thread);

/* Release stack resources owned by a userspace thread that is no longer running. */
bool uthread_deinit(struct uthread* thread);
