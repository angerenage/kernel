#pragma once

#include <base/vmm.h>
#include <core/process.h>
#include <core/thread.h>
#include <core/user_upcall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t uthread_id_t;

#define UTHREAD_ID_INVALID ((uthread_id_t)0u)

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
	UTHREAD_START_ID_EXHAUSTED,
};

struct uthread_start_params {
	const char*     name;
	struct process* process;
	uintptr_t       user_entry;
	const void*     arg_data;
	size_t          arg_size;
	size_t          user_stack_pages;
	struct cpu*     preferred_cpu;
	bool            detached;
};

struct uthread {
	struct thread            thread;
	uthread_id_t             id;
	struct process*          process;
	struct user_upcall_state upcall;
	vmm_id_t                 user_stack_id;
	vmm_id_t                 kernel_stack_id;
	uintptr_t                user_stack_top;
	struct uthread*          reaper_next;
	struct uthread*          process_next;
	/* Lazily-created cap_object id for this thread. */
	cap_object_id_t cap_object_id;
	/* References keep the descriptor and its owned resources alive. */
	uint64_t reference_count;
	/* Set before the ID is removed and the owner reference is released. */
	uint32_t dying;
	bool     heap_allocated;
};

/* Initialize caller-owned userspace thread storage and queue it on the scheduler. */
enum uthread_start_result uthread_start(struct uthread* thread, const struct uthread_start_params* params);

/*
 * Allocate, initialize, and queue a detached userspace thread.
 * The process remains caller-owned; the thread descriptor and stacks are
 * reclaimed by the user-thread reaper after exit.
 */
enum uthread_start_result uthread_spawn_detached(struct uthread**                   out_thread,
                                                 const struct uthread_start_params* params);

/* Return a userspace thread ID, or UTHREAD_ID_INVALID for NULL. */
uthread_id_t uthread_id(const struct uthread* thread);

/* Return the userspace thread that owns a scheduler thread, or NULL. */
struct uthread* uthread_from_thread(struct thread* thread);

/* Return the userspace thread currently associated with the running CPU, or NULL. */
struct uthread* uthread_current(void);

/*
 * Return the userspace thread registered for id, or NULL when id is invalid or absent.
 * The returned pointer is borrowed and requires external lifetime synchronization.
 */
struct uthread* uthread_lookup(uthread_id_t id);

/* Look up and retain id. The caller must release the returned descriptor. */
struct uthread* uthread_acquire(uthread_id_t id);

/* Drop a reference acquired through uthread_acquire(). */
void uthread_release(struct uthread* thread);

/* Return the number of registered userspace threads. */
size_t uthread_count(void);

/* Convert a live joinable userspace thread into a detached thread. */
bool uthread_detach(struct uthread* thread);

/* Release stack resources owned by a userspace thread that is no longer running. */
bool uthread_deinit(struct uthread* thread);

/* Return the lazily-created cap_object id for thread. */
cap_object_id_t uthread_cap_object_id(const struct uthread* thread);

/* Publish a cap_object id on thread. */
void uthread_set_cap_object_id(struct uthread* thread, cap_object_id_t id);

/* Destroy the lazily-created cap_object for thread and clear the slot. */
bool uthread_destroy_cap_object(struct uthread* thread);
