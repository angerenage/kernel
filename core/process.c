#include <core/kheap.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct spinlock process_pid_lock =
	SPINLOCK_INIT_CLASS("process_pid_lock", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);
static process_id_t process_next_pid = 1u;

static bool process_alloc_pid(process_id_t* out_pid) {
	process_id_t     pid;
	struct irq_state state;

	if (out_pid == NULL) return false;
	*out_pid = PROCESS_PID_INVALID;

	state = spinlock_lock_irqsave(&process_pid_lock);
	pid   = process_next_pid;
	if (pid == PROCESS_PID_INVALID) {
		spinlock_unlock_irqrestore(&process_pid_lock, state);
		return false;
	}
	process_next_pid++;
	if (process_next_pid == PROCESS_PID_INVALID) process_next_pid = PROCESS_PID_INVALID;
	spinlock_unlock_irqrestore(&process_pid_lock, state);

	*out_pid = pid;
	return true;
}

static enum process_result process_create(struct process** out_process, const char* name) {
	struct process* process;
	process_id_t    pid;

	if (out_process == NULL) return PROCESS_INVALID_ARGUMENTS;
	*out_process = NULL;

	if (!process_alloc_pid(&pid)) return PROCESS_PID_EXHAUSTED;

	process = kmalloc(sizeof(*process));
	if (process == NULL) return PROCESS_NO_MEMORY;

	memset(process, 0, sizeof(*process));
	process->pid   = pid;
	process->name  = name;
	process->state = PROCESS_STATE_NEW;
	spinlock_init_class(&process->lock, "process", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);
	thread_wait_queue_init(&process->join_wait_queue);

	if (!vmm_user_address_space_init(&process->address_space)) {
		kfree(process);
		return PROCESS_ADDRESS_SPACE_FAILED;
	}

	*out_process = process;
	return PROCESS_OK;
}

static enum process_result process_result_from_uthread(enum uthread_start_result result) {
	switch (result) {
	case UTHREAD_START_OK:
		return PROCESS_OK;
	case UTHREAD_START_INVALID_ARGUMENTS:
		return PROCESS_INVALID_ARGUMENTS;
	case UTHREAD_START_NO_MEMORY:
		return PROCESS_NO_MEMORY;
	case UTHREAD_START_STACK_ALLOC_FAILED:
		return PROCESS_THREAD_STACK_ALLOC_FAILED;
	case UTHREAD_START_CONTEXT_UNSUPPORTED:
		return PROCESS_THREAD_CONTEXT_UNSUPPORTED;
	case UTHREAD_START_SCHEDULER_REJECTED:
		return PROCESS_THREAD_SCHEDULER_REJECTED;
	case UTHREAD_START_REAPER_UNAVAILABLE:
		return PROCESS_THREAD_REAPER_UNAVAILABLE;
	case UTHREAD_START_ID_EXHAUSTED:
	default:
		return PROCESS_THREAD_ID_EXHAUSTED;
	}
}

static bool process_all_threads_terminated_locked(const struct process* process) {
	const struct uthread* cursor;

	if (process == NULL || process->thread_head == NULL) return true;

	cursor = process->thread_head;
	while (cursor != NULL) {
		if (!thread_is_terminated(&cursor->thread)) return false;
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

enum process_result process_create_thread(struct process* process, struct uthread* thread,
                                          const struct process_thread_params* params) {
	enum uthread_start_result result;

	if (process == NULL || thread == NULL || params == NULL) return PROCESS_INVALID_ARGUMENTS;

	result = uthread_start(thread,
	                       &(const struct uthread_start_params){
							   .name             = params->name,
							   .process          = process,
							   .user_entry       = params->user_entry,
							   .user_arg         = params->user_arg,
							   .user_stack_pages = params->user_stack_pages,
							   .preferred_cpu    = params->preferred_cpu,
							   .detached         = params->detached,
						   });
	return process_result_from_uthread(result);
}

bool process_terminate(struct process* process, uintptr_t exit_code) {
	struct irq_state state;
	struct irq_state wait_state;
	struct uthread*  cursor;
	struct thread*   current;
	bool             current_in_process = false;
	bool             wake_joiners       = false;

	if (process == NULL) return false;

	wait_state = spinlock_lock_irqsave(&process->join_wait_queue.lock);
	state      = spinlock_lock_irqsave(&process->lock);
	if (process->state == PROCESS_STATE_ZOMBIE) {
		spinlock_unlock_irqrestore(&process->lock, state);
		spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);
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

	cursor = process->thread_head;
	while (cursor != NULL) {
		struct uthread* next = cursor->process_next;

		if (&cursor->thread != current) (void)thread_request_cancel(&cursor->thread);
		cursor = next;
	}

	if (wake_joiners) (void)sched_wake_all(&process->join_wait_queue);
	if (current_in_process) sched_exit_current(exit_code);
	return true;
}

enum process_join_result process_join(struct process* process, uintptr_t* out_exit_code) {
	struct thread* current;

	if (process == NULL) return PROCESS_JOIN_INVALID_ARGUMENTS;

	current = sched_current_thread();
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

	if (process == NULL) return PROCESS_DETACH_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&process->lock);
	if (process->joined) {
		spinlock_unlock_irqrestore(&process->lock, state);
		return PROCESS_DETACH_ALREADY_JOINED;
	}
	if (process->detached) {
		spinlock_unlock_irqrestore(&process->lock, state);
		return PROCESS_DETACH_ALREADY_DETACHED;
	}

	process->detached = true;
	spinlock_unlock_irqrestore(&process->lock, state);
	return PROCESS_DETACH_OK;
}

enum process_result process_spawn(struct process** out_process, const struct process_spawn_params* params) {
	struct process*     process = NULL;
	struct uthread*     main_thread;
	enum process_result create_result;
	enum process_result thread_result;

	if (out_process == NULL || params == NULL || params->user_entry == 0u) return PROCESS_INVALID_ARGUMENTS;
	*out_process = NULL;

	create_result = process_create(&process, params->name);
	if (create_result != PROCESS_OK) return create_result;

	main_thread = kmalloc(sizeof(*main_thread));
	if (main_thread == NULL) {
		(void)process_destroy(process);
		return PROCESS_NO_MEMORY;
	}
	*main_thread = (struct uthread){
		.user_stack_id   = VMM_ID_INVALID,
		.kernel_stack_id = VMM_ID_INVALID,
	};

	thread_result = process_create_thread(process,
	                                      main_thread,
	                                      &(const struct process_thread_params){
											  .name             = params->name,
											  .user_entry       = params->user_entry,
											  .user_arg         = params->user_arg,
											  .user_stack_pages = params->user_stack_pages,
											  .preferred_cpu    = params->preferred_cpu,
											  .detached         = false,
										  });
	if (thread_result != PROCESS_OK) {
		kfree(main_thread);
		(void)process_destroy(process);
		return thread_result;
	}

	main_thread->heap_allocated = true;
	process->main_thread        = main_thread;
	*out_process                = process;
	return PROCESS_OK;
}

bool process_destroy(struct process* process) {
	struct irq_state state;
	struct uthread*  cursor;

	if (process == NULL) return false;

	state  = spinlock_lock_irqsave(&process->lock);
	cursor = process->thread_head;
	while (cursor != NULL) {
		if ((!thread_is_terminated(&cursor->thread) && cursor->thread.state != THREAD_STATE_NEW) ||
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

	vmm_address_space_deinit(&process->address_space);
	memset(process, 0, sizeof(*process));
	kfree(process);
	return true;
}

process_id_t process_pid(const struct process* process) {
	return process == NULL ? PROCESS_PID_INVALID : process->pid;
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
	struct thread* current = sched_current_thread();
	return current == NULL ? NULL : current->process;
}

void process_notify_thread_exit(struct process* process, struct thread* thread, uintptr_t exit_code) {
	struct irq_state state;
	struct irq_state wait_state;
	bool             wake_joiners;

	if (process == NULL || thread == NULL) return;

	wait_state = spinlock_lock_irqsave(&process->join_wait_queue.lock);
	state      = spinlock_lock_irqsave(&process->lock);
	if (thread->process == process && process->state != PROCESS_STATE_EXITING &&
	    process->state != PROCESS_STATE_ZOMBIE) {
		process->exit_code = exit_code;
	}
	wake_joiners = process_mark_zombie_if_complete_locked(process);
	spinlock_unlock_irqrestore(&process->lock, state);
	spinlock_unlock_irqrestore(&process->join_wait_queue.lock, wait_state);

	if (wake_joiners) (void)sched_wake_all(&process->join_wait_queue);
}
