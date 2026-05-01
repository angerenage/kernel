#include <core/kheap.h>
#include <core/process.h>
#include <core/spinlock.h>
#include <core/uthread.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum process_create_result {
	PROCESS_CREATE_OK = 0,
	PROCESS_CREATE_INVALID_ARGUMENTS,
	PROCESS_CREATE_NO_MEMORY,
	PROCESS_CREATE_ADDRESS_SPACE_FAILED,
	PROCESS_CREATE_PID_EXHAUSTED,
};

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

static enum process_create_result process_create(struct process** out_process, const char* name) {
	struct process* process;
	process_id_t    pid;

	if (out_process == NULL) return PROCESS_CREATE_INVALID_ARGUMENTS;
	*out_process = NULL;

	if (!process_alloc_pid(&pid)) return PROCESS_CREATE_PID_EXHAUSTED;

	process = kmalloc(sizeof(*process));
	if (process == NULL) return PROCESS_CREATE_NO_MEMORY;

	memset(process, 0, sizeof(*process));
	process->pid   = pid;
	process->name  = name;
	process->state = PROCESS_STATE_NEW;
	spinlock_init_class(&process->lock, "process", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);

	if (!vmm_user_address_space_init(&process->address_space)) {
		kfree(process);
		return PROCESS_CREATE_ADDRESS_SPACE_FAILED;
	}

	*out_process = process;
	return PROCESS_CREATE_OK;
}

static enum process_spawn_result process_spawn_result_from_create(enum process_create_result result) {
	switch (result) {
	case PROCESS_CREATE_OK:
		return PROCESS_SPAWN_OK;
	case PROCESS_CREATE_INVALID_ARGUMENTS:
		return PROCESS_SPAWN_INVALID_ARGUMENTS;
	case PROCESS_CREATE_NO_MEMORY:
	case PROCESS_CREATE_ADDRESS_SPACE_FAILED:
	case PROCESS_CREATE_PID_EXHAUSTED:
	default:
		return PROCESS_SPAWN_CREATE_FAILED;
	}
}

enum process_spawn_result process_spawn(struct process** out_process, const struct process_spawn_params* params) {
	struct process*            process = NULL;
	struct uthread*            main_thread;
	enum process_create_result create_result;
	enum uthread_start_result  thread_result;

	if (out_process == NULL || params == NULL || params->user_entry == 0u) return PROCESS_SPAWN_INVALID_ARGUMENTS;
	*out_process = NULL;

	create_result = process_create(&process, params->name);
	if (create_result != PROCESS_CREATE_OK) return process_spawn_result_from_create(create_result);

	main_thread = kmalloc(sizeof(*main_thread));
	if (main_thread == NULL) {
		(void)process_destroy(process);
		return PROCESS_SPAWN_CREATE_FAILED;
	}
	*main_thread = (struct uthread){
		.user_stack_id   = VMM_ID_INVALID,
		.kernel_stack_id = VMM_ID_INVALID,
	};

	thread_result = uthread_start(main_thread,
	                              &(const struct uthread_start_params){
									  .name             = params->name,
									  .process          = process,
									  .user_entry       = params->user_entry,
									  .user_arg         = params->user_arg,
									  .user_stack_pages = params->user_stack_pages,
									  .preferred_cpu    = params->preferred_cpu,
									  .detached         = false,
								  });
	if (thread_result != UTHREAD_START_OK) {
		kfree(main_thread);
		(void)process_destroy(process);
		return PROCESS_SPAWN_MAIN_THREAD_FAILED;
	}

	main_thread->heap_allocated = true;
	process->main_thread        = main_thread;
	*out_process                = process;
	return PROCESS_SPAWN_OK;
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
