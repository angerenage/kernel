#include <core/thread.h>

static void thread_reset_links(struct thread* thread) {
	if (thread == NULL) return;

	thread->run_queue_next = NULL;
	thread->flags &= ~THREAD_FLAG_QUEUED;
}

void thread_init(struct thread* thread, const char* name, uintptr_t kernel_stack_base, uintptr_t kernel_stack_top) {
	if (thread == NULL) return;

	*thread = (struct thread){
		.name              = name,
		.cpu               = NULL,
		.state             = THREAD_STATE_NEW,
		.flags             = THREAD_FLAG_NONE,
		.kernel_stack_base = kernel_stack_base,
		.kernel_stack_top  = kernel_stack_top,
		.context           = {0},
		.run_queue_next    = NULL,
	};
}

void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name) {
	if (thread == NULL) return;

	thread_init(thread, name, 0u, 0u);
	thread->cpu   = cpu;
	thread->state = THREAD_STATE_IDLE;
	thread->flags = THREAD_FLAG_IDLE;
}

bool thread_is_idle(const struct thread* thread) {
	return thread != NULL && (thread->flags & THREAD_FLAG_IDLE) != 0u;
}

bool thread_is_queued(const struct thread* thread) {
	return thread != NULL && (thread->flags & THREAD_FLAG_QUEUED) != 0u;
}

void thread_mark_running(struct thread* thread, struct cpu* cpu) {
	if (thread == NULL) return;

	thread_reset_links(thread);
	thread->cpu = cpu;
	if (!thread_is_idle(thread)) thread->state = THREAD_STATE_RUNNING;
}

void thread_mark_blocked(struct thread* thread) {
	if (thread == NULL) return;

	thread_reset_links(thread);
	if (!thread_is_idle(thread)) thread->state = THREAD_STATE_BLOCKED;
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

	if (queue == NULL || thread == NULL || thread_is_idle(thread)) return false;

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
	queue->tail            = thread;
	thread->run_queue_next = NULL;
	thread->flags |= THREAD_FLAG_QUEUED;
	thread->state = THREAD_STATE_READY;
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
