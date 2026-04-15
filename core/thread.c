#include <core/sched.h>
#include <core/thread.h>

static void thread_reset_links(struct thread* thread) {
	if (thread == NULL) return;

	thread->run_queue_next     = NULL;
	thread->wait_queue_next    = NULL;
	thread->sleep_queue_next   = NULL;
	thread->wake_deadline_tick = 0u;
	thread->flags &= ~THREAD_FLAG_QUEUED;
}

__attribute__((noreturn))
static void thread_entry_bootstrap(void* ctx) {
	struct thread* thread = (struct thread*)ctx;

	if (thread != NULL && thread->entry != NULL) thread->entry(thread->arg);
	sched_exit_current(0u);
}

bool thread_init(struct thread* thread, const struct thread_create_params* params) {
	if (thread == NULL || params == NULL || params->entry == NULL) return false;
	if (params->kernel_stack_top <= params->kernel_stack_base) return false;

	*thread = (struct thread){
		.name              = params->name,
		.cpu               = NULL,
		.preferred_cpu     = params->preferred_cpu,
		.state             = THREAD_STATE_NEW,
		.block_reason      = THREAD_BLOCK_NONE,
		.flags             = params->detached ? THREAD_FLAG_DETACHED : THREAD_FLAG_NONE,
		.kernel_stack_base = params->kernel_stack_base,
		.kernel_stack_top  = params->kernel_stack_top,
		.context =
			{
					  .instruction_pointer = (uintptr_t)params->entry,
					  .stack_pointer       = params->kernel_stack_top,
					  },
		.entry              = params->entry,
		.arg                = params->arg,
		.exit_code          = 0u,
		.blocked_queue      = NULL,
		.run_queue_next     = NULL,
		.wait_queue_next    = NULL,
		.sleep_queue_next   = NULL,
		.wake_deadline_tick = 0u,
		.wait_status        = THREAD_WAIT_STATUS_NONE,
	};
	thread_wait_queue_init(&thread->join_wait_queue);
	return hal_cpu_thread_context_init(&thread->context,
	                                   params->kernel_stack_base,
	                                   params->kernel_stack_top,
	                                   (uintptr_t)thread_entry_bootstrap,
	                                   (uintptr_t)thread);
}

void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name) {
	if (thread == NULL) return;

	*thread = (struct thread){
		.name               = name,
		.cpu                = cpu,
		.preferred_cpu      = cpu,
		.state              = THREAD_STATE_IDLE,
		.block_reason       = THREAD_BLOCK_NONE,
		.flags              = THREAD_FLAG_IDLE,
		.kernel_stack_base  = 0u,
		.kernel_stack_top   = 0u,
		.context            = {0},
		.entry              = NULL,
		.arg                = NULL,
		.exit_code          = 0u,
		.blocked_queue      = NULL,
		.run_queue_next     = NULL,
		.wait_queue_next    = NULL,
		.sleep_queue_next   = NULL,
		.wake_deadline_tick = 0u,
		.wait_status        = THREAD_WAIT_STATUS_NONE,
	};
	thread_wait_queue_init(&thread->join_wait_queue);
}

bool thread_is_idle(const struct thread* thread) {
	return thread != NULL && (thread->flags & THREAD_FLAG_IDLE) != 0u;
}

bool thread_is_queued(const struct thread* thread) {
	return thread != NULL && (thread->flags & THREAD_FLAG_QUEUED) != 0u;
}

bool thread_is_terminated(const struct thread* thread) {
	if (thread == NULL) return false;
	return thread->state == THREAD_STATE_EXITING || thread->state == THREAD_STATE_ZOMBIE;
}

bool thread_is_joinable(const struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread)) return false;
	return (thread->flags & THREAD_FLAG_DETACHED) == 0u;
}

bool thread_cancel_requested(const struct thread* thread) {
	return thread != NULL && (thread->flags & THREAD_FLAG_CANCEL_PENDING) != 0u;
}

bool thread_detach(struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread) || thread->state == THREAD_STATE_ZOMBIE) return false;
	if (!thread_is_joinable(thread)) return false;

	thread->flags |= THREAD_FLAG_DETACHED;
	return true;
}

bool thread_request_cancel(struct thread* thread) {
	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;

	thread->flags |= THREAD_FLAG_CANCEL_PENDING;
	return true;
}

void thread_set_cancel_enabled(struct thread* thread, bool enabled) {
	if (thread == NULL) return;

	if (enabled) {
		thread->flags &= ~THREAD_FLAG_CANCEL_DISABLED;
	}
	else {
		thread->flags |= THREAD_FLAG_CANCEL_DISABLED;
	}
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
	if (!thread_is_idle(thread) && !thread_is_terminated(thread)) thread->state = THREAD_STATE_RUNNING;
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
	if (thread == NULL || thread_is_idle(thread)) return;

	thread_reset_links(thread);
	thread->blocked_queue = NULL;
	thread->wait_status   = THREAD_WAIT_STATUS_NONE;
	thread->block_reason  = THREAD_BLOCK_NONE;
	thread->exit_code     = exit_code;
	thread->state         = THREAD_STATE_EXITING;
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
	if (queue->tail == NULL) {
		queue->head = thread;
	}
	else {
		queue->tail->run_queue_next = thread;
	}
	queue->tail             = thread;
	thread->run_queue_next  = NULL;
	thread->wait_queue_next = NULL;
	thread->flags |= THREAD_FLAG_QUEUED;
	thread->block_reason = THREAD_BLOCK_NONE;
	thread->state        = THREAD_STATE_READY;
	queue->depth++;
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
