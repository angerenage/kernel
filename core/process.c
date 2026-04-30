#include <core/kheap.h>
#include <core/process.h>
#include <core/spinlock.h>
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

enum process_create_result process_create(struct process** out_process, const char* name) {
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

bool process_destroy(struct process* process) {
	struct irq_state state;
	bool             can_destroy;

	if (process == NULL) return false;

	state       = spinlock_lock_irqsave(&process->lock);
	can_destroy = process->thread_count == 0u;
	spinlock_unlock_irqrestore(&process->lock, state);
	if (!can_destroy) return false;

	vmm_address_space_deinit(&process->address_space);
	memset(process, 0, sizeof(*process));
	kfree(process);
	return true;
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
