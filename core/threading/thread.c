#include <core/sched.h>
#include <core/thread.h>

int32_t thread_priority_clamp(int32_t priority) {
	if (priority < THREAD_PRIORITY_MIN) return THREAD_PRIORITY_MIN;
	if (priority > THREAD_PRIORITY_MAX) return THREAD_PRIORITY_MAX;
	return priority;
}

static uint32_t thread_flags_load(const struct thread* thread) {
	return thread == NULL ? THREAD_FLAG_NONE : __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
}

static void thread_reset_links(struct thread* thread) {
	if (thread == NULL) return;

	thread->run_queue_next     = NULL;
	thread->wait_queue_next    = NULL;
	thread->sleep_queue_next   = NULL;
	thread->wake_deadline_tick = 0u;
	(void)__atomic_fetch_and(
		&thread->flags, ~((uint32_t)THREAD_FLAG_QUEUED | (uint32_t)THREAD_FLAG_WAIT_INTERRUPTIBLE), __ATOMIC_ACQ_REL);
}

static void thread_exit_if_cancelled(struct thread* thread) {
	if (!thread_should_cancel(thread)) return;

	sched_exit_current(THREAD_EXIT_CODE_CANCELLED);
}

__attribute__((noreturn))
static void thread_entry_bootstrap(void* ctx) {
	struct thread* thread = (struct thread*)ctx;

	sched_complete_context_switch();
	thread_exit_if_cancelled(thread);
	if (thread != NULL && thread->entry != NULL) thread->entry(thread->arg);
	sched_exit_current(0u);
}

static enum thread_init_result thread_validate_create_params(const struct thread_create_params* params) {
	if (params == NULL || params->entry == NULL) return THREAD_INIT_INVALID_ARGUMENTS;
	if (params->kernel_stack_top <= params->kernel_stack_base) return THREAD_INIT_INVALID_STACK;
	return THREAD_INIT_OK;
}

static enum thread_init_result thread_validate_context_params(const struct thread_context_params* params) {
	if (params == NULL || params->context == NULL) return THREAD_INIT_INVALID_ARGUMENTS;
	if (params->kernel_stack_top <= params->kernel_stack_base) return THREAD_INIT_INVALID_STACK;
	return THREAD_INIT_OK;
}

enum thread_init_result thread_init_context(struct thread* thread, const struct thread_context_params* params) {
	enum thread_init_result result;
	int32_t                 priority;

	if (thread == NULL) return THREAD_INIT_INVALID_ARGUMENTS;

	result = thread_validate_context_params(params);
	if (result != THREAD_INIT_OK) return result;

	priority = thread_priority_clamp(params->base_priority);
	*thread  = (struct thread){
		 .name                = params->name,
		 .cpu                 = NULL,
		 .preferred_cpu       = params->preferred_cpu,
		 .state               = THREAD_STATE_NEW,
		 .block_reason        = THREAD_BLOCK_NONE,
		 .flags               = params->detached ? THREAD_FLAG_DETACHED : THREAD_FLAG_NONE,
		 .kernel_stack_base   = params->kernel_stack_base,
		 .kernel_stack_top    = params->kernel_stack_top,
		 .address_space       = params->address_space,
		 .owner_kind          = THREAD_OWNER_NONE,
		 .owner               = NULL,
		 .context             = *params->context,
		 .entry               = params->entry,
		 .arg                 = params->arg,
		 .exit_code           = 0u,
		 .blocked_queue       = NULL,
		 .owned_mutexes       = NULL,
		 .run_queue_next      = NULL,
		 .wait_queue_next     = NULL,
		 .sleep_queue_next    = NULL,
		 .wake_deadline_tick  = 0u,
		 .wait_status         = THREAD_WAIT_STATUS_NONE,
		 .base_priority       = priority,
		 .effective_priority  = priority,
		 .timeslice_ticks     = THREAD_DEFAULT_TIMESLICE_TICKS,
		 .timeslice_remaining = THREAD_DEFAULT_TIMESLICE_TICKS,
		 .reap_callback       = NULL,
		 .reap_context        = NULL,
    };
	thread_wait_queue_init(&thread->join_wait_queue);
	thread_wait_queue_init(&thread->park_wait_queue);
	return THREAD_INIT_OK;
}

enum thread_init_result thread_init_ex(struct thread* thread, const struct thread_create_params* params) {
	enum thread_init_result      result;
	struct thread_context        context;
	struct thread_context_params context_params;

	if (thread == NULL) return THREAD_INIT_INVALID_ARGUMENTS;

	result = thread_validate_create_params(params);
	if (result != THREAD_INIT_OK) return result;

	if (!hal_cpu_thread_context_init(&context,
	                                 params->kernel_stack_base,
	                                 params->kernel_stack_top,
	                                 (uintptr_t)thread_entry_bootstrap,
	                                 (uintptr_t)thread)) {
		return THREAD_INIT_CONTEXT_UNSUPPORTED;
	}

	context_params = (struct thread_context_params){
		.name              = params->name,
		.kernel_stack_base = params->kernel_stack_base,
		.kernel_stack_top  = params->kernel_stack_top,
		.address_space     = params->address_space,
		.preferred_cpu     = params->preferred_cpu,
		.base_priority     = params->base_priority,
		.detached          = params->detached,
		.context           = &context,
		.entry             = params->entry,
		.arg               = params->arg,
	};
	return thread_init_context(thread, &context_params);
}

bool thread_init(struct thread* thread, const struct thread_create_params* params) {
	return thread_init_ex(thread, params) == THREAD_INIT_OK;
}

void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name) {
	if (thread == NULL) return;

	*thread = (struct thread){
		.name                = name,
		.cpu                 = cpu,
		.preferred_cpu       = cpu,
		.state               = THREAD_STATE_IDLE,
		.block_reason        = THREAD_BLOCK_NONE,
		.flags               = THREAD_FLAG_IDLE,
		.kernel_stack_base   = 0u,
		.kernel_stack_top    = 0u,
		.address_space       = NULL,
		.owner_kind          = THREAD_OWNER_NONE,
		.owner               = NULL,
		.context             = {0},
		.entry               = NULL,
		.arg                 = NULL,
		.exit_code           = 0u,
		.blocked_queue       = NULL,
		.owned_mutexes       = NULL,
		.run_queue_next      = NULL,
		.wait_queue_next     = NULL,
		.sleep_queue_next    = NULL,
		.wake_deadline_tick  = 0u,
		.wait_status         = THREAD_WAIT_STATUS_NONE,
		.base_priority       = THREAD_PRIORITY_MIN,
		.effective_priority  = THREAD_PRIORITY_MIN,
		.timeslice_ticks     = 0u,
		.timeslice_remaining = 0u,
		.reap_callback       = NULL,
		.reap_context        = NULL,
	};
	hal_cpu_fp_context_init(&thread->context.fp_context);
	thread_wait_queue_init(&thread->join_wait_queue);
	thread_wait_queue_init(&thread->park_wait_queue);
}

bool thread_is_idle(const struct thread* thread) {
	return (thread_flags_load(thread) & THREAD_FLAG_IDLE) != 0u;
}

bool thread_is_queued(const struct thread* thread) {
	return (thread_flags_load(thread) & THREAD_FLAG_QUEUED) != 0u;
}

bool thread_is_terminated(const struct thread* thread) {
	if (thread == NULL) return false;
	return thread->state == THREAD_STATE_EXITING || thread->state == THREAD_STATE_ZOMBIE;
}

bool thread_is_reap_safe(const struct thread* thread) {
	return thread != NULL && thread->state == THREAD_STATE_ZOMBIE;
}

bool thread_is_joinable(const struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread)) return false;
	return (thread_flags_load(thread) & THREAD_FLAG_DETACHED) == 0u;
}

bool thread_cancel_requested(const struct thread* thread) {
	return (thread_flags_load(thread) & THREAD_FLAG_CANCEL_PENDING) != 0u;
}

bool thread_cancel_enabled(const struct thread* thread) {
	return thread != NULL && (thread_flags_load(thread) & THREAD_FLAG_CANCEL_DISABLED) == 0u;
}

bool thread_should_cancel(const struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;

	return thread_cancel_requested(thread) && thread_cancel_enabled(thread);
}

void thread_request_interrupt(struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return;

	(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_INTERRUPT_PENDING, __ATOMIC_ACQ_REL);
}

bool thread_interrupt_pending(const struct thread* thread) {
	return (thread_flags_load(thread) & THREAD_FLAG_INTERRUPT_PENDING) != 0u;
}

void thread_clear_interrupt(struct thread* thread) {
	if (thread == NULL) return;

	(void)__atomic_fetch_and(&thread->flags, ~(uint32_t)THREAD_FLAG_INTERRUPT_PENDING, __ATOMIC_ACQ_REL);
}

bool thread_detach(struct thread* thread) {
	struct irq_state state;

	if (thread == NULL || thread_is_idle(thread)) return false;

	state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);
	if (thread_is_terminated(thread) || !thread_is_joinable(thread)) {
		spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
		return false;
	}
	(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_DETACHED, __ATOMIC_ACQ_REL);
	spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
	return true;
}

bool thread_detach_with_reap_callback(struct thread* thread, thread_reap_callback_t callback, void* ctx) {
	struct irq_state state;

	if (thread == NULL || thread_is_idle(thread)) return false;

	state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);
	if (thread_is_terminated(thread) || !thread_is_joinable(thread)) {
		spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
		return false;
	}
	thread->reap_callback = callback;
	thread->reap_context  = ctx;
	(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_DETACHED, __ATOMIC_ACQ_REL);
	spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
	return true;
}

bool thread_request_cancel(struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;

	(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_CANCEL_PENDING, __ATOMIC_ACQ_REL);
	sched_cancel_thread(thread);
	return true;
}

void thread_set_cancel_enabled(struct thread* thread, bool enabled) {
	uint32_t previous_flags;

	if (thread == NULL) return;

	if (enabled) {
		previous_flags = __atomic_fetch_and(&thread->flags, ~(uint32_t)THREAD_FLAG_CANCEL_DISABLED, __ATOMIC_ACQ_REL);
		if ((previous_flags & (THREAD_FLAG_CANCEL_DISABLED | THREAD_FLAG_CANCEL_PENDING)) ==
		    (THREAD_FLAG_CANCEL_DISABLED | THREAD_FLAG_CANCEL_PENDING)) {
			sched_cancel_thread(thread);
		}
	}
	else {
		(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_CANCEL_DISABLED, __ATOMIC_ACQ_REL);
	}
}

void thread_set_reap_callback(struct thread* thread, thread_reap_callback_t callback, void* ctx) {
	struct irq_state state;

	if (thread == NULL) return;

	state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);
	if (!thread_is_terminated(thread)) {
		thread->reap_callback = callback;
		thread->reap_context  = ctx;
	}
	spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
}

void thread_notify_reap(struct thread* thread) {
	thread_reap_callback_t callback;
	void*                  ctx;
	struct irq_state       state;

	if (thread == NULL) return;

	state    = spinlock_lock_irqsave(&thread->join_wait_queue.lock);
	callback = thread->reap_callback;
	ctx      = thread->reap_context;
	spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
	if (callback != NULL) callback(thread, ctx);
}

void thread_mark_ready(struct thread* thread, struct cpu* cpu) {
	if (thread == NULL) return;

	thread_reset_links(thread);
	thread->cpu           = cpu;
	thread->blocked_queue = NULL;
	thread->block_reason  = THREAD_BLOCK_NONE;
	if (!thread_is_idle(thread) && !thread_is_terminated(thread)) thread->state = THREAD_STATE_READY;
}

void thread_mark_running(struct thread* thread, struct cpu* cpu) {
	if (thread == NULL) return;

	thread_reset_links(thread);
	thread->cpu           = cpu;
	thread->blocked_queue = NULL;
	thread->block_reason  = THREAD_BLOCK_NONE;
	if (!thread_is_idle(thread) && !thread_is_terminated(thread)) {
		if (thread->timeslice_ticks == 0u) thread->timeslice_ticks = THREAD_DEFAULT_TIMESLICE_TICKS;
		if (thread->timeslice_remaining == 0u) thread->timeslice_remaining = thread->timeslice_ticks;
		thread->state = THREAD_STATE_RUNNING;
	}
}

void thread_mark_blocked(struct thread* thread, enum thread_block_reason reason) {
	if (thread == NULL) return;

	thread_reset_links(thread);
	thread->blocked_queue = NULL;
	thread->wait_status   = THREAD_WAIT_STATUS_NONE;
	thread->block_reason  = reason;
	if (!thread_is_idle(thread) && !thread_is_terminated(thread)) thread->state = THREAD_STATE_BLOCKED;
}

void thread_mark_exiting(struct thread* thread, thread_exit_code_t exit_code) {
	struct irq_state state;

	if (thread == NULL || thread_is_idle(thread)) return;

	state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);
	thread_reset_links(thread);
	thread->blocked_queue = NULL;
	thread->wait_status   = THREAD_WAIT_STATUS_NONE;
	thread->block_reason  = THREAD_BLOCK_NONE;
	thread->exit_code     = exit_code;
	thread->state         = THREAD_STATE_EXITING;
	spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, state);
}

void thread_mark_zombie(struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread)) return;

	thread_reset_links(thread);
	thread->blocked_queue = NULL;
	thread->wait_status   = THREAD_WAIT_STATUS_NONE;
	thread->block_reason  = THREAD_BLOCK_NONE;
	thread->state         = THREAD_STATE_ZOMBIE;
}

void thread_wait_queue_init(struct thread_wait_queue* queue) {
	if (queue == NULL) return;

	queue->lock =
		(struct spinlock)SPINLOCK_INIT_CLASS("thread_wait_queue_lock", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE);
	queue->head  = NULL;
	queue->tail  = NULL;
	queue->depth = 0u;
}

size_t thread_wait_queue_depth(struct thread_wait_queue* queue) {
	struct irq_state state;
	size_t           depth;

	if (queue == NULL) return 0u;

	state = spinlock_lock_irqsave(&queue->lock);
	depth = queue->depth;
	spinlock_unlock_irqrestore(&queue->lock, state);
	return depth;
}

static void run_queue_insert_locked(struct run_queue* queue, struct thread* thread) {
	struct thread* previous = NULL;
	struct thread* current;

	if (queue == NULL || thread == NULL) return;

	current = queue->head;
	while (current != NULL && current->effective_priority >= thread->effective_priority) {
		previous = current;
		current  = current->run_queue_next;
	}

	if (previous == NULL) {
		thread->run_queue_next = queue->head;
		queue->head            = thread;
	}
	else {
		thread->run_queue_next   = previous->run_queue_next;
		previous->run_queue_next = thread;
	}

	if (thread->run_queue_next == NULL) queue->tail = thread;
	if (queue->tail == NULL) queue->tail = thread;
}

static bool run_queue_remove_locked(struct run_queue* queue, struct thread* thread) {
	struct thread* previous = NULL;
	struct thread* current;

	if (queue == NULL || thread == NULL) return false;

	current = queue->head;
	while (current != NULL && current != thread) {
		previous = current;
		current  = current->run_queue_next;
	}

	if (current == NULL) return false;

	if (previous == NULL) {
		queue->head = current->run_queue_next;
	}
	else {
		previous->run_queue_next = current->run_queue_next;
	}

	if (queue->tail == current) queue->tail = previous;
	current->run_queue_next = NULL;
	return true;
}

void run_queue_init(struct run_queue* queue) {
	if (queue == NULL) return;

	queue->lock  = (struct spinlock)SPINLOCK_INIT_CLASS("run_queue_lock", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE);
	queue->head  = NULL;
	queue->tail  = NULL;
	queue->depth = 0u;
}

bool run_queue_enqueue(struct run_queue* queue, struct thread* thread) {
	struct irq_state state;

	if (queue == NULL || thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;

	state = spinlock_lock_irqsave(&queue->lock);
	if (thread_is_queued(thread)) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return false;
	}

	run_queue_insert_locked(queue, thread);
	thread->wait_queue_next = NULL;
	(void)__atomic_fetch_or(&thread->flags, (uint32_t)THREAD_FLAG_QUEUED, __ATOMIC_ACQ_REL);
	thread->block_reason = THREAD_BLOCK_NONE;
	thread->state        = THREAD_STATE_READY;
	queue->depth++;
	spinlock_unlock_irqrestore(&queue->lock, state);
	return true;
}

bool run_queue_requeue(struct run_queue* queue, struct thread* thread) {
	struct irq_state state;

	if (queue == NULL || thread == NULL || !thread_is_queued(thread)) return false;

	state = spinlock_lock_irqsave(&queue->lock);
	if (!run_queue_remove_locked(queue, thread)) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return false;
	}
	run_queue_insert_locked(queue, thread);
	spinlock_unlock_irqrestore(&queue->lock, state);
	return true;
}

bool run_queue_update_priority(struct run_queue* queue, struct thread* thread, int32_t priority) {
	struct irq_state state;

	if (queue == NULL || thread == NULL) return false;

	state = spinlock_lock_irqsave(&queue->lock);
	if (thread->effective_priority == priority) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return true;
	}
	if (!run_queue_remove_locked(queue, thread)) {
		/* The scheduler may have dequeued the thread after the caller observed
		 * THREAD_FLAG_QUEUED. The queue no longer needs reordering, but the
		 * requested priority change must still take effect. */
		thread->effective_priority = priority;
		spinlock_unlock_irqrestore(&queue->lock, state);
		return false;
	}
	thread->effective_priority = priority;
	run_queue_insert_locked(queue, thread);
	spinlock_unlock_irqrestore(&queue->lock, state);
	return true;
}

struct thread* run_queue_dequeue(struct run_queue* queue) {
	struct irq_state state;
	struct thread*   thread;

	if (queue == NULL) return NULL;

	state  = spinlock_lock_irqsave(&queue->lock);
	thread = queue->head;
	if (thread == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return NULL;
	}

	queue->head = thread->run_queue_next;
	if (queue->head == NULL) queue->tail = NULL;
	thread_reset_links(thread);
	if (queue->depth != 0u) queue->depth--;
	spinlock_unlock_irqrestore(&queue->lock, state);
	return thread;
}

size_t run_queue_depth(struct run_queue* queue) {
	struct irq_state state;
	size_t           depth;

	if (queue == NULL) return 0u;

	state = spinlock_lock_irqsave(&queue->lock);
	depth = queue->depth;
	spinlock_unlock_irqrestore(&queue->lock, state);
	return depth;
}
