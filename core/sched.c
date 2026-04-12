#include <core/cpu.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <hal/cpu.h>
#include <stdio.h>

#define SCHED_MAX_CPU_COUNT 64u
#define SCHED_IDLE_NAME_MAX 16u

struct sched_cpu_state {
	bool             present;
	struct run_queue run_queue;
	struct thread    idle_thread;
	char             idle_name[SCHED_IDLE_NAME_MAX];
};

static struct sched_cpu_state sched_cpu_state[SCHED_MAX_CPU_COUNT];

static struct sched_cpu_state* sched_state_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return NULL;
	if (!sched_cpu_state[cpu->index].present) return NULL;
	return &sched_cpu_state[cpu->index];
}

static struct cpu* sched_default_cpu(void) {
	struct cpu* cpu = cpu_current();

	if (sched_state_for_cpu(cpu) != NULL) return cpu;

	cpu = cpu_bsp();
	if (sched_state_for_cpu(cpu) != NULL) return cpu;

	for (size_t i = 0; i < cpu_count(); i++) {
		cpu = cpu_by_index(i);
		if (sched_state_for_cpu(cpu) != NULL) return cpu;
	}

	return NULL;
}

static struct cpu* sched_target_cpu_for_thread(const struct thread* thread) {
	struct cpu* candidates[4];

	if (thread == NULL) return NULL;

	candidates[0] = thread->preferred_cpu;
	candidates[1] = thread->cpu;
	candidates[2] = cpu_current();
	candidates[3] = cpu_bsp();

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (sched_state_for_cpu(candidates[i]) != NULL) return candidates[i];
	}

	return sched_default_cpu();
}

static bool sched_wait_queue_enqueue_locked(struct thread_wait_queue* queue, struct thread* thread) {
	if (queue == NULL || thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;
	if (thread->wait_queue_next != NULL) return false;

	if (queue->tail == NULL) {
		queue->head = thread;
	}
	else {
		queue->tail->wait_queue_next = thread;
	}

	queue->tail             = thread;
	thread->wait_queue_next = NULL;
	queue->depth++;
	return true;
}

static struct thread* sched_wait_queue_dequeue_locked(struct thread_wait_queue* queue) {
	struct thread* thread;

	if (queue == NULL) return NULL;

	thread = queue->head;
	if (thread == NULL) return NULL;

	queue->head = thread->wait_queue_next;
	if (queue->head == NULL) queue->tail = NULL;
	thread->wait_queue_next = NULL;
	if (queue->depth != 0u) queue->depth--;
	return thread;
}

static bool sched_run_queue_remove_locked(struct run_queue* queue, struct thread* thread) {
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
	current->flags &= ~THREAD_FLAG_QUEUED;
	if (queue->depth != 0u) queue->depth--;
	return true;
}

static struct thread* sched_select_next(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	struct thread*          next;

	if (state == NULL) return NULL;

	next = run_queue_dequeue(&state->run_queue);
	if (next == NULL) return &state->idle_thread;

	return next;
}

static void sched_dispatch_next(struct cpu* cpu) {
	struct thread* next;
	struct thread* previous;

	if (cpu == NULL) return;

	previous = cpu->current_thread;
	next     = sched_select_next(cpu);
	if (next == NULL) return;

	if (previous == next) {
		sched_set_current(cpu, next);
		return;
	}

	sched_set_current(cpu, next);
	if (previous != NULL) hal_cpu_context_switch(&previous->context, &next->context);
}

static bool sched_make_runnable_on_cpu(struct cpu* cpu, struct thread* thread, bool allow_current) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);

	if (state == NULL || thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;
	if (!allow_current && cpu != NULL && cpu->current_thread == thread) return false;

	thread_mark_ready(thread, cpu);
	return run_queue_enqueue(&state->run_queue, thread);
}

bool sched_init(void) {
	size_t cpu_total = cpu_count();

	if (cpu_total == 0u || cpu_total > SCHED_MAX_CPU_COUNT) return false;

	for (size_t i = 0; i < SCHED_MAX_CPU_COUNT; i++) {
		sched_cpu_state[i] = (struct sched_cpu_state){0};
	}

	for (size_t i = 0; i < cpu_total; i++) {
		struct cpu*             cpu = cpu_by_index(i);
		struct sched_cpu_state* state;

		if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return false;

		state          = &sched_cpu_state[cpu->index];
		state->present = true;
		run_queue_init(&state->run_queue);
		sprintf(state->idle_name, "idle/%zu", cpu->index);
		thread_init_idle(&state->idle_thread, cpu, state->idle_name);
		cpu->current_thread = NULL;
	}

	return true;
}

bool sched_start_cpu(struct cpu* cpu) {
	struct thread* idle = sched_idle_thread(cpu);

	if (idle == NULL) return false;
	sched_set_current(cpu, idle);
	return true;
}

void sched_set_current(struct cpu* cpu, struct thread* thread) {
	if (cpu == NULL) return;

	cpu->current_thread = thread;
	if (thread != NULL) thread_mark_running(thread, cpu);
}

struct thread* sched_current_thread(void) {
	struct cpu* cpu = cpu_current();
	return cpu == NULL ? NULL : cpu->current_thread;
}

struct thread* sched_idle_thread(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	return state == NULL ? NULL : &state->idle_thread;
}

bool sched_make_runnable(struct thread* thread) {
	struct cpu* cpu = sched_target_cpu_for_thread(thread);

	return sched_make_runnable_on_cpu(cpu, thread, false);
}

bool sched_remove_runnable(struct thread* thread) {
	struct sched_cpu_state* state;
	struct irq_state        irq_state;
	struct cpu*             cpu;
	bool                    removed;

	if (thread == NULL || !thread_is_queued(thread)) return false;

	cpu   = thread->cpu;
	state = sched_state_for_cpu(cpu);
	if (state == NULL) return false;

	irq_state = spinlock_lock_irqsave(&state->run_queue.lock);
	removed   = sched_run_queue_remove_locked(&state->run_queue, thread);
	spinlock_unlock_irqrestore(&state->run_queue.lock, irq_state);

	if (!removed) return false;

	thread_mark_ready(thread, cpu);
	return true;
}

void sched_yield(void) {
	struct cpu*    cpu = cpu_current();
	struct thread* current;

	if (cpu == NULL) return;

	current = cpu->current_thread;
	if (current == NULL) {
		(void)sched_start_cpu(cpu);
		current = cpu->current_thread;
	}

	if (current != NULL && !thread_is_idle(current) && !thread_is_terminated(current)) {
		(void)sched_make_runnable_on_cpu(cpu, current, true);
	}

	sched_dispatch_next(cpu);
}

bool sched_block_current_locked(struct thread_wait_queue* queue, enum thread_block_reason reason,
                                struct irq_state queue_irq_state) {
	struct cpu*    cpu = cpu_current();
	struct thread* current;
	bool           queued;

	if (queue == NULL) return false;
	if (cpu == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return false;
	}

	current = cpu->current_thread;
	if (current == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		(void)sched_start_cpu(cpu);
		return false;
	}
	if (thread_is_idle(current) || thread_is_terminated(current)) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return false;
	}

	if (reason == THREAD_BLOCK_NONE) reason = THREAD_BLOCK_WAIT_QUEUE;

	thread_mark_blocked(current, reason);
	queued = sched_wait_queue_enqueue_locked(queue, current);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued) {
		thread_mark_running(current, cpu);
		return false;
	}

	sched_dispatch_next(cpu);
	return true;
}

void sched_block_current(struct thread_wait_queue* queue, enum thread_block_reason reason) {
	struct irq_state irq_state;

	if (queue == NULL) return;

	irq_state = spinlock_lock_irqsave(&queue->lock);
	(void)sched_block_current_locked(queue, reason, irq_state);
}

bool sched_wake_one(struct thread_wait_queue* queue) {
	struct irq_state irq_state;
	struct thread*   thread;

	if (queue == NULL) return false;

	irq_state = spinlock_lock_irqsave(&queue->lock);
	thread    = sched_wait_queue_dequeue_locked(queue);
	spinlock_unlock_irqrestore(&queue->lock, irq_state);

	if (thread == NULL) return false;
	return sched_make_runnable(thread);
}

size_t sched_wake_all(struct thread_wait_queue* queue) {
	struct irq_state irq_state;
	struct thread*   thread;
	size_t           woken = 0u;

	if (queue == NULL) return 0u;

	irq_state    = spinlock_lock_irqsave(&queue->lock);
	thread       = queue->head;
	queue->head  = NULL;
	queue->tail  = NULL;
	queue->depth = 0u;
	spinlock_unlock_irqrestore(&queue->lock, irq_state);

	while (thread != NULL) {
		struct thread* next = thread->wait_queue_next;

		thread->wait_queue_next = NULL;
		if (sched_make_runnable(thread)) woken++;
		thread = next;
	}

	return woken;
}

void sched_exit_current(thread_exit_code_t exit_code) {
	struct cpu*    cpu     = cpu_current();
	struct thread* current = sched_current_thread();

	if (cpu == NULL || current == NULL || thread_is_idle(current)) {
		for (;;) {
			hal_cpu_park();
		}
	}

	thread_mark_exiting(current, exit_code);
	(void)sched_wake_all(&current->join_wait_queue);
	thread_mark_zombie(current);
	sched_dispatch_next(cpu);

	for (;;) {
		hal_cpu_park();
	}
}

size_t sched_run_queue_depth(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	return state == NULL ? 0u : run_queue_depth(&state->run_queue);
}

void sched_enter_idle(void) {
	struct cpu* cpu = cpu_current();

	if (!sched_start_cpu(cpu)) {
		for (;;) {
			hal_cpu_park();
		}
	}

	for (;;) {
		if (sched_run_queue_depth(cpu) != 0u) {
			sched_yield();
			continue;
		}

		hal_cpu_park();
	}
}
