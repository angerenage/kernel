#include <core/capability.h>
#include <core/capability_call.h>
#include <core/id_table.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vm_space.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct id_table process_table = {
	.lock    = SPINLOCK_INIT_CLASS("process_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = 1u,
	.min_id  = 1u,
	.max_id  = UINT64_MAX,
};

static enum process_thread_spawn_result process_thread_spawn_result_from_uthread(enum uthread_start_result result) {
	switch (result) {
	case UTHREAD_START_OK:
		return PROCESS_THREAD_SPAWN_OK;
	case UTHREAD_START_INVALID_ARGUMENTS:
		return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	case UTHREAD_START_NO_MEMORY:
		return PROCESS_THREAD_SPAWN_NO_MEMORY;
	case UTHREAD_START_STACK_ALLOC_FAILED:
		return PROCESS_THREAD_SPAWN_STACK_ALLOC_FAILED;
	case UTHREAD_START_CONTEXT_UNSUPPORTED:
		return PROCESS_THREAD_SPAWN_CONTEXT_UNSUPPORTED;
	case UTHREAD_START_SCHEDULER_REJECTED:
		return PROCESS_THREAD_SPAWN_SCHEDULER_REJECTED;
	case UTHREAD_START_REAPER_UNAVAILABLE:
		return PROCESS_THREAD_SPAWN_REAPER_UNAVAILABLE;
	case UTHREAD_START_ID_EXHAUSTED:
	default:
		return PROCESS_THREAD_SPAWN_ID_EXHAUSTED;
	}
}

static bool process_all_threads_terminated_locked(const struct process* process) {
	const struct uthread* cursor;

	if (process == NULL || process->thread_head == NULL) return true;

	cursor = process->thread_head;
	while (cursor != NULL) {
		if (!thread_is_reap_safe(&cursor->thread)) return false;
		cursor = cursor->process_next;
	}
	return true;
}

static bool process_mark_zombie_if_complete_locked(struct process* process) {
	if (process == NULL || process->state == PROCESS_STATE_ZOMBIE) return false;
	if (!process_all_threads_terminated_locked(process)) return false;

	process->state = PROCESS_STATE_ZOMBIE;
	return true;
}

static bool process_has_thread_locked(const struct process* process, const struct uthread* thread) {
	const struct uthread* cursor;

	if (process == NULL || thread == NULL) return false;

	cursor = process->thread_head;
	while (cursor != NULL) {
		if (cursor == thread) return true;
		cursor = cursor->process_next;
	}
	return false;
}

static bool process_has_thread(struct process* process, struct uthread* thread) {
	struct irq_state state;
	bool             found;

	if (process == NULL || thread == NULL) return false;

	state = spinlock_lock_irqsave(&process->lock);
	found = process_has_thread_locked(process, thread) && thread->process == process;
	spinlock_unlock_irqrestore(&process->lock, state);
	return found;
}

static enum process_thread_spawn_result process_prepare_thread_internal(struct process*                     process,
                                                                        struct uthread**                    out_thread,
                                                                        const struct process_thread_params* params,
                                                                        bool main_thread) {
	struct uthread*           thread;
	enum uthread_start_result start_result;

	if (process == NULL || params == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	if (out_thread == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	*out_thread = NULL;

	thread = malloc(sizeof(*thread));
	if (thread == NULL) return PROCESS_THREAD_SPAWN_NO_MEMORY;
	start_result = uthread_prepare(thread,
	                               &(const struct uthread_start_params){
									   .name             = params->name,
									   .process          = process,
									   .user_entry       = params->user_entry,
									   .arg_data         = params->arg_data,
									   .arg_size         = params->arg_size,
									   .user_stack_pages = params->user_stack_pages,
									   .preferred_cpu    = params->preferred_cpu,
									   .detached         = params->detached,
									   .main_thread      = main_thread,
								   });
	if (start_result != UTHREAD_START_OK) {
		free(thread);
		return process_thread_spawn_result_from_uthread(start_result);
	}

	thread->heap_allocated = true;
	*out_thread            = thread;
	return PROCESS_THREAD_SPAWN_OK;
}

enum process_thread_spawn_result process_prepare_thread(struct process* process, struct uthread** out_thread,
                                                        const struct process_thread_params* params) {
	return process_prepare_thread_internal(process, out_thread, params, false);
}

enum process_thread_spawn_result process_commit_thread(struct process* process, struct uthread* thread) {
	enum uthread_start_result result;

	if (process == NULL || thread == NULL || thread->process != process || thread->process_main_thread) {
		return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	}
	result = uthread_commit_prepared(thread, false);
	return process_thread_spawn_result_from_uthread(result);
}

bool process_abort_thread(struct process* process, struct uthread* thread) {
	if (process == NULL || thread == NULL || thread->process != process || thread->process_main_thread ||
	    process_has_thread(process, thread)) {
		return false;
	}
	return uthread_abort_prepared(thread);
}

enum process_thread_spawn_result process_spawn_thread(struct process* process, struct uthread** out_thread,
                                                      const struct process_thread_params* params) {
	struct uthread*                  prepared = NULL;
	enum process_thread_spawn_result result;

	if (out_thread == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	*out_thread = NULL;
	result      = process_prepare_thread(process, &prepared, params);
	if (result != PROCESS_THREAD_SPAWN_OK) return result;

	result = process_commit_thread(process, prepared);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_abort_thread(process, prepared);
		return result;
	}

	*out_thread = params->detached ? NULL : prepared;
	return PROCESS_THREAD_SPAWN_OK;
}

static void process_release_main_thread_claim(struct process* process) {
	struct irq_state state;

	if (process == NULL) return;
	state                         = spinlock_lock_irqsave(&process->lock);
	process->main_thread_starting = false;
	spinlock_unlock_irqrestore(&process->lock, state);
}

enum process_thread_spawn_result process_prepare_main_thread(struct process* process, struct uthread** out_thread,
                                                             const struct process_thread_params* params) {
	struct irq_state                 state;
	enum process_thread_spawn_result result;
	bool                             claimed;

	if (process == NULL || out_thread == NULL || params == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	*out_thread = NULL;

	state   = spinlock_lock_irqsave(&process->lock);
	claimed = process->state == PROCESS_STATE_NEW && process->main_thread == NULL && process->thread_count == 0u &&
	          !process->main_thread_starting;
	if (claimed) process->main_thread_starting = true;
	spinlock_unlock_irqrestore(&process->lock, state);
	if (!claimed) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;

	result = process_prepare_thread_internal(process, out_thread, params, true);
	if (result != PROCESS_THREAD_SPAWN_OK) process_release_main_thread_claim(process);
	return result;
}

enum process_thread_spawn_result process_commit_main_thread(struct process* process, struct uthread* thread) {
	struct irq_state          state;
	bool                      claimed;
	enum uthread_start_result start_result;

	if (process == NULL || thread == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	state   = spinlock_lock_irqsave(&process->lock);
	claimed = process->main_thread_starting && process->main_thread == NULL && process->thread_count == 0u &&
	          thread->process == process;
	spinlock_unlock_irqrestore(&process->lock, state);
	if (!claimed) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;

	start_result = uthread_commit_prepared(thread, true);
	if (start_result != UTHREAD_START_OK) return process_thread_spawn_result_from_uthread(start_result);

	process_release_main_thread_claim(process);
	return PROCESS_THREAD_SPAWN_OK;
}

bool process_abort_main_thread(struct process* process, struct uthread* thread) {
	struct irq_state state;
	bool             claimed;

	if (process == NULL || thread == NULL || !thread->process_main_thread) return false;
	state   = spinlock_lock_irqsave(&process->lock);
	claimed = process->main_thread_starting && process->main_thread == NULL && process->thread_count == 0u &&
	          thread->process == process;
	spinlock_unlock_irqrestore(&process->lock, state);
	if (!claimed || !uthread_abort_prepared(thread)) return false;

	process_release_main_thread_claim(process);
	return true;
}

enum process_thread_spawn_result process_start_main_thread(struct process* process, struct uthread** out_thread,
                                                           const struct process_thread_params* params) {
	struct uthread*                  prepared = NULL;
	enum process_thread_spawn_result result;

	if (out_thread == NULL) return PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS;
	*out_thread = NULL;
	result      = process_prepare_main_thread(process, &prepared, params);
	if (result != PROCESS_THREAD_SPAWN_OK) return result;

	result = process_commit_main_thread(process, prepared);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_abort_main_thread(process, prepared);
		return result;
	}

	*out_thread = params->detached ? NULL : prepared;
	return PROCESS_THREAD_SPAWN_OK;
}

enum process_thread_join_result process_join_thread(struct process* process, struct uthread* thread,
                                                    uintptr_t* out_exit_code) {
	struct thread* current;
	uintptr_t      exit_code;

	if (process == NULL || thread == NULL) return PROCESS_THREAD_JOIN_INVALID_ARGUMENTS;

	current = sched_current_thread();
	if (current == &thread->thread) return PROCESS_THREAD_JOIN_SELF;
	if (!process_has_thread(process, thread)) return PROCESS_THREAD_JOIN_FOREIGN_THREAD;
	if (!thread_is_joinable(&thread->thread)) return PROCESS_THREAD_JOIN_DETACHED;

	if (!thread_is_reap_safe(&thread->thread)) {
		struct irq_state wait_state = spinlock_lock_irqsave(&thread->thread.join_wait_queue.lock);

		if (!thread_is_reap_safe(&thread->thread)) {
			if (!sched_block_current_locked(&thread->thread.join_wait_queue, THREAD_BLOCK_JOIN, wait_state)) {
				return PROCESS_THREAD_JOIN_WAIT_FAILED;
			}
		}
		else {
			spinlock_unlock_irqrestore(&thread->thread.join_wait_queue.lock, wait_state);
		}
	}

	if (!process_has_thread(process, thread)) return PROCESS_THREAD_JOIN_FOREIGN_THREAD;
	if (!thread_is_reap_safe(&thread->thread)) return PROCESS_THREAD_JOIN_WAIT_FAILED;

	exit_code = thread->thread.exit_code;
	if (!uthread_deinit(thread)) return PROCESS_THREAD_JOIN_RECLAIM_FAILED;
	if (out_exit_code != NULL) *out_exit_code = exit_code;
	return PROCESS_THREAD_JOIN_OK;
}

enum process_thread_detach_result process_detach_thread(struct process* process, struct uthread* thread) {
	if (process == NULL || thread == NULL) return PROCESS_THREAD_DETACH_INVALID_ARGUMENTS;
	if (!process_has_thread(process, thread)) return PROCESS_THREAD_DETACH_FOREIGN_THREAD;
	if (thread_is_terminated(&thread->thread)) return PROCESS_THREAD_DETACH_ALREADY_TERMINATED;
	if (!thread_is_joinable(&thread->thread)) return PROCESS_THREAD_DETACH_ALREADY_DETACHED;

	return uthread_detach(thread) ? PROCESS_THREAD_DETACH_OK : PROCESS_THREAD_DETACH_FAILED;
}

enum process_thread_cancel_result process_cancel_thread(struct process* process, struct uthread* thread) {
	if (process == NULL || thread == NULL) return PROCESS_THREAD_CANCEL_INVALID_ARGUMENTS;
	if (!process_has_thread(process, thread)) return PROCESS_THREAD_CANCEL_FOREIGN_THREAD;
	if (thread_is_terminated(&thread->thread)) return PROCESS_THREAD_CANCEL_ALREADY_TERMINATED;

	return thread_request_cancel(&thread->thread) ? PROCESS_THREAD_CANCEL_OK : PROCESS_THREAD_CANCEL_FAILED;
}

enum process_result process_create(struct process** out_process, const char* name) {
	struct process*      process;
	process_id_t         pid;
	enum id_table_result id_result;

	if (out_process == NULL) return PROCESS_INVALID_ARGUMENTS;
	*out_process = NULL;

	process = malloc(sizeof(*process));
	if (process == NULL) return PROCESS_NO_MEMORY;

	memset(process, 0, sizeof(*process));
	if (name != NULL) {
		process->name = strdup(name);
		if (process->name == NULL) {
			free(process);
			return PROCESS_NO_MEMORY;
		}
	}
	process->state                       = PROCESS_STATE_NEW;
	process->cap_object_id               = CAP_OBJECT_ID_INVALID;
	process->address_space_cap_object_id = CAP_OBJECT_ID_INVALID;
	process->reference_count             = 1u;
	spinlock_init_class(&process->lock, "process", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);
	thread_wait_queue_init(&process->join_wait_queue);
	if (!message_queue_init(&process->message_queue)) {
		free((void*)process->name);
		free(process);
		return PROCESS_NO_MEMORY;
	}
	process_channel_state_init(&process->channel_state);

	if (!vm_space_create_user(&process->address_space)) {
		ring_buffer_deinit(&process->message_queue);
		free((void*)process->name);
		free(process);
		return PROCESS_ADDRESS_SPACE_FAILED;
	}

	id_result = id_table_alloc(&process_table, process, &pid);
	if (id_result != ID_TABLE_OK) {
		vm_space_destroy(&process->address_space);
		ring_buffer_deinit(&process->message_queue);
		free((void*)process->name);
		free(process);
		return id_result == ID_TABLE_NO_MEMORY ? PROCESS_NO_MEMORY : PROCESS_PID_EXHAUSTED;
	}
	process->pid = pid;

	*out_process = process;
	return PROCESS_OK;
}

bool process_terminate(struct process* process, uintptr_t exit_code) {
	enum { CANCEL_BATCH_SIZE = 16u };
	struct irq_state state;
	struct irq_state wait_state;
	struct uthread*  cursor;
	struct uthread*  cancel_batch[CANCEL_BATCH_SIZE];
	struct process*  held;
	struct thread*   current;
	bool             current_in_process = false;
	bool             wake_joiners       = false;

	if (process == NULL) return false;

	held = process_acquire(process_pid(process));
	if (held != process) {
		if (held != NULL) process_release(held);
		return false;
	}
	process = held;

	wait_state = spinlock_lock_irqsave(&process->join_wait_queue.lock);
	state      = spinlock_lock_irqsave(&process->lock);
	if (process->state == PROCESS_STATE_ZOMBIE) {
		spinlock_unlock_irqrestore(&process->lock, state);
		spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
		process_release(process);
		return false;
	}

	if (process->state != PROCESS_STATE_EXITING) {
		process->exit_code = exit_code;
		process->state     = PROCESS_STATE_EXITING;
	}

	current = sched_current_thread();
	cursor  = process->thread_head;
	while (cursor != NULL) {
		if (&cursor->thread == current) current_in_process = true;
		cursor = cursor->process_next;
	}

	wake_joiners = process_mark_zombie_if_complete_locked(process);
	spinlock_unlock_irqrestore(&process->lock, state);
	spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
	cap_pending_call_cancel_caller(process->pid);
	cap_pending_call_cancel_provider(process->pid);

	for (;;) {
		size_t cancel_count = 0u;

		state  = spinlock_lock_irqsave(&process->lock);
		cursor = process->thread_head;
		while (cursor != NULL && cancel_count < CANCEL_BATCH_SIZE) {
			if (&cursor->thread != current && !thread_is_terminated(&cursor->thread) &&
			    !thread_cancel_requested(&cursor->thread) && uthread_retain(cursor)) {
				cancel_batch[cancel_count++] = cursor;
			}
			cursor = cursor->process_next;
		}
		spinlock_unlock_irqrestore(&process->lock, state);

		if (cancel_count == 0u) break;
		for (size_t i = 0u; i < cancel_count; i++) {
			(void)thread_request_cancel(&cancel_batch[i]->thread);
			uthread_release(cancel_batch[i]);
		}
	}

	if (wake_joiners) {
		(void)sched_wake_all(&process->join_wait_queue);
		(void)process_reap_detached(process);
	}
	if (current_in_process) {
		process_release(process);
		sched_exit_current(exit_code);
	}
	process_release(process);
	return true;
}

enum process_join_result process_join(struct process* process, uintptr_t* out_exit_code) {
	struct uthread* current;

	if (process == NULL) return PROCESS_JOIN_INVALID_ARGUMENTS;

	current = uthread_current();
	if (current != NULL && current->process == process) return PROCESS_JOIN_SELF;

	for (;;) {
		struct irq_state wait_state;
		struct irq_state state;
		uintptr_t        exit_code;

		wait_state = spinlock_lock_irqsave(&process->join_wait_queue.lock);
		state      = spinlock_lock_irqsave(&process->lock);

		(void)process_mark_zombie_if_complete_locked(process);
		if (process->detached) {
			spinlock_unlock_irqrestore(&process->lock, state);
			spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
			return PROCESS_JOIN_DETACHED;
		}
		if (process->joined) {
			spinlock_unlock_irqrestore(&process->lock, state);
			spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
			return PROCESS_JOIN_ALREADY_JOINED;
		}
		if (process->state == PROCESS_STATE_ZOMBIE) {
			process->joined = true;
			exit_code       = process->exit_code;
			spinlock_unlock_irqrestore(&process->lock, state);
			spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
			if (out_exit_code != NULL) *out_exit_code = exit_code;
			return PROCESS_JOIN_OK;
		}

		spinlock_unlock_irqrestore(&process->lock, state);
		if (!sched_block_current_locked(&process->join_wait_queue, THREAD_BLOCK_JOIN, wait_state)) {
			return PROCESS_JOIN_WAIT_FAILED;
		}
	}
}

enum process_detach_result process_detach(struct process* process) {
	struct irq_state state;
	struct process*  held;

	if (process == NULL) return PROCESS_DETACH_INVALID_ARGUMENTS;

	held = process_acquire(process_pid(process));
	if (held != process) {
		if (held != NULL) process_release(held);
		return PROCESS_DETACH_INVALID_ARGUMENTS;
	}
	process = held;

	state = spinlock_lock_irqsave(&process->lock);
	if (process->joined) {
		spinlock_unlock_irqrestore(&process->lock, state);
		process_release(process);
		return PROCESS_DETACH_ALREADY_JOINED;
	}
	if (process->detached) {
		spinlock_unlock_irqrestore(&process->lock, state);
		process_release(process);
		return PROCESS_DETACH_ALREADY_DETACHED;
	}

	process->detached = true;
	spinlock_unlock_irqrestore(&process->lock, state);
	(void)process_reap_detached(process);
	process_release(process);
	return PROCESS_DETACH_OK;
}

static void process_finalize(struct process* process) {
	(void)process_destroy_address_space_cap_object(process);
	(void)process_destroy_cap_object(process);
	vm_space_destroy(&process->address_space);
	process_channel_state_deinit(&process->channel_state);
	ring_buffer_deinit(&process->message_queue);
	free((void*)process->name);
	memset(process, 0, sizeof(*process));
	free(process);
}

void process_release(struct process* process) {
	if (process == NULL) return;
	if (__atomic_sub_fetch(&process->reference_count, 1u, __ATOMIC_ACQ_REL) == 0u) process_finalize(process);
}

bool process_reap_detached(struct process* process) {
	struct irq_state state;
	bool             ready;

	if (process == NULL) return false;
	state = spinlock_lock_irqsave(&process->lock);
	ready = process->detached && process->state == PROCESS_STATE_ZOMBIE && !process->main_thread_starting &&
	        process_all_threads_terminated_locked(process);
	spinlock_unlock_irqrestore(&process->lock, state);
	return ready && process_destroy(process);
}

bool process_destroy(struct process* process) {
	struct irq_state state;
	struct uthread*  cursor;
	struct process*  removed;

	if (process == NULL) return false;

	state = spinlock_lock_irqsave(&process->lock);
	if (process->main_thread_starting) {
		spinlock_unlock_irqrestore(&process->lock, state);
		return false;
	}
	cursor = process->thread_head;
	while (cursor != NULL) {
		if ((!thread_is_reap_safe(&cursor->thread) && cursor->thread.state != THREAD_STATE_NEW) ||
		    !thread_is_joinable(&cursor->thread)) {
			spinlock_unlock_irqrestore(&process->lock, state);
			return false;
		}
		cursor = cursor->process_next;
	}
	spinlock_unlock_irqrestore(&process->lock, state);

	for (;;) {
		struct uthread* thread;

		state  = spinlock_lock_irqsave(&process->lock);
		thread = process->thread_head;
		spinlock_unlock_irqrestore(&process->lock, state);
		if (thread == NULL) break;
		if (!uthread_deinit(thread)) return false;
	}

	removed = NULL;
	cap_object_cleanup_for_process(process->pid);
	if (id_table_remove(&process_table, process->pid, (void**)&removed) != ID_TABLE_OK || removed != process) {
		return false;
	}
	cap_drop_for_process(process->pid);
	process_release(removed);
	return true;
}

process_id_t process_pid(const struct process* process) {
	return process == NULL ? PROCESS_PID_INVALID : process->pid;
}

struct process* process_lookup(process_id_t pid) {
	return (struct process*)id_table_lookup(&process_table, pid);
}

static bool process_retain_callback(void* value, void* context) {
	struct process* process = value;
	uint64_t        current;

	(void)context;
	current = __atomic_load_n(&process->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&process->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

struct process* process_acquire(process_id_t pid) {
	if (pid == PROCESS_PID_INVALID) return NULL;
	return id_table_lookup_retain(&process_table, pid, process_retain_callback, NULL);
}

size_t process_count(void) {
	return id_table_count(&process_table);
}

struct address_space* process_address_space(struct process* process) {
	return process == NULL ? NULL : &process->address_space;
}

struct uthread* process_main_thread(struct process* process) {
	struct uthread*  main_thread;
	struct irq_state state;

	if (process == NULL) return NULL;

	state       = spinlock_lock_irqsave(&process->lock);
	main_thread = process->main_thread;
	spinlock_unlock_irqrestore(&process->lock, state);
	return main_thread;
}

enum process_state process_get_state(struct process* process) {
	enum process_state state;
	struct irq_state   irq_state;

	if (process == NULL) return PROCESS_STATE_ZOMBIE;

	irq_state = spinlock_lock_irqsave(&process->lock);
	state     = process->state;
	spinlock_unlock_irqrestore(&process->lock, irq_state);
	return state;
}

size_t process_thread_count(struct process* process) {
	size_t           count;
	struct irq_state state;

	if (process == NULL) return 0u;

	state = spinlock_lock_irqsave(&process->lock);
	count = process->thread_count;
	spinlock_unlock_irqrestore(&process->lock, state);
	return count;
}

struct process* process_current(void) {
	struct uthread* current = uthread_current();
	return current == NULL ? NULL : current->process;
}

void process_notify_thread_exit(struct process* process, struct thread* thread, uintptr_t exit_code) {
	struct irq_state state;
	struct irq_state wait_state;
	struct uthread*  uthread;
	bool             wake_joiners;

	if (process == NULL || thread == NULL) return;

	uthread    = uthread_from_thread(thread);
	wait_state = spinlock_lock_irqsave(&process->join_wait_queue.lock);
	state      = spinlock_lock_irqsave(&process->lock);
	if (uthread != NULL && uthread->process == process && process->state != PROCESS_STATE_EXITING &&
	    process->state != PROCESS_STATE_ZOMBIE) {
		process->exit_code = exit_code;
	}
	wake_joiners = process_mark_zombie_if_complete_locked(process);
	spinlock_unlock_irqrestore(&process->lock, state);
	spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);

	if (wake_joiners) {
		cap_pending_call_cancel_caller(process->pid);
		cap_pending_call_cancel_provider(process->pid);
		(void)sched_wake_all(&process->join_wait_queue);
	}
}

cap_object_id_t process_cap_object_id(const struct process* process) {
	return process == NULL ? CAP_OBJECT_ID_INVALID : process->cap_object_id;
}

void process_set_cap_object_id(struct process* process, cap_object_id_t id) {
	struct irq_state state;

	if (process == NULL) return;

	state                  = spinlock_lock_irqsave(&process->lock);
	process->cap_object_id = id;
	spinlock_unlock_irqrestore(&process->lock, state);
}

bool process_destroy_cap_object(struct process* process) {
	struct irq_state state;
	cap_object_id_t  id;
	bool             destroyed;

	if (process == NULL) return false;

	state                  = spinlock_lock_irqsave(&process->lock);
	id                     = process->cap_object_id;
	process->cap_object_id = CAP_OBJECT_ID_INVALID;
	spinlock_unlock_irqrestore(&process->lock, state);

	if (id == CAP_OBJECT_ID_INVALID) return false;

	destroyed = cap_object_destroy_with_id(id);
	return destroyed;
}

cap_object_id_t process_address_space_cap_object_id(const struct process* process) {
	return process == NULL ? CAP_OBJECT_ID_INVALID : process->address_space_cap_object_id;
}

void process_set_address_space_cap_object_id(struct process* process, cap_object_id_t id) {
	struct irq_state state;

	if (process == NULL) return;

	state                                = spinlock_lock_irqsave(&process->lock);
	process->address_space_cap_object_id = id;
	spinlock_unlock_irqrestore(&process->lock, state);
}

bool process_destroy_address_space_cap_object(struct process* process) {
	struct irq_state state;
	cap_object_id_t  id;

	if (process == NULL) return false;

	state                                = spinlock_lock_irqsave(&process->lock);
	id                                   = process->address_space_cap_object_id;
	process->address_space_cap_object_id = CAP_OBJECT_ID_INVALID;
	spinlock_unlock_irqrestore(&process->lock, state);

	return id != CAP_OBJECT_ID_INVALID && cap_object_destroy_with_id(id);
}
