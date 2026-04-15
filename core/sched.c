#include <core/cpu.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <hal/cpu.h>
#include <stdio.h>

#define SCHED_MAX_CPU_COUNT 64u
#define SCHED_IDLE_NAME_MAX 16u

enum sched_cpu_activity {
	SCHED_CPU_ACTIVITY_KERNEL = 0,
	SCHED_CPU_ACTIVITY_THREAD,
	SCHED_CPU_ACTIVITY_IDLE,
};

struct sched_cpu_state {
	bool                   present;
	struct run_queue       run_queue;
	struct thread          idle_thread;
	char                   idle_name[SCHED_IDLE_NAME_MAX];
	uint32_t               activity;
	struct sched_cpu_stats stats;
};

static struct sched_cpu_state sched_cpu_state[SCHED_MAX_CPU_COUNT];
static struct spinlock        sched_sleep_lock =
	(struct spinlock)SPINLOCK_INIT_CLASS("sched_sleep_lock", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE);
static struct thread*     sched_sleep_head;
static uint64_t           sched_ticks;
static struct sched_stats sched_stats;

static void sched_stat_increment(uint64_t* counter) {
	(void)__atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

static void sched_clear_reschedule_request(struct cpu* cpu) {
	if (cpu == NULL) return;

	__atomic_store_n(&cpu->reschedule_requested, false, __ATOMIC_RELEASE);
}

static void sched_charge_current_timeslice(struct cpu* cpu) {
	struct thread* current;

	if (cpu == NULL) return;

	current = cpu->current_thread;
	if (current == NULL || thread_is_idle(current) || thread_is_terminated(current) || current->timeslice_ticks == 0u) {
		return;
	}

	if (current->timeslice_remaining == 0u) current->timeslice_remaining = current->timeslice_ticks;
	current->timeslice_remaining--;
	if (current->timeslice_remaining != 0u) return;

	current->timeslice_remaining = current->timeslice_ticks;
	if (sched_run_queue_depth(cpu) != 0u) sched_request_reschedule(cpu);
}

static struct sched_cpu_state* sched_state_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return NULL;
	if (!sched_cpu_state[cpu->index].present) return NULL;
	return &sched_cpu_state[cpu->index];
}

static void sched_stat_increment_pair(uint64_t* global_counter, uint64_t* cpu_counter) {
	sched_stat_increment(global_counter);
	if (cpu_counter != NULL) sched_stat_increment(cpu_counter);
}

static enum sched_cpu_activity sched_activity_for_thread(const struct thread* thread) {
	if (thread == NULL) return SCHED_CPU_ACTIVITY_KERNEL;
	return thread_is_idle(thread) ? SCHED_CPU_ACTIVITY_IDLE : SCHED_CPU_ACTIVITY_THREAD;
}

static void sched_set_cpu_activity(struct cpu* cpu, enum sched_cpu_activity activity) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);

	if (state == NULL) return;

	__atomic_store_n(&state->activity, (uint32_t)activity, __ATOMIC_RELEASE);
}

static void sched_account_cpu_tick(struct cpu* cpu) {
	struct sched_cpu_state* state;
	enum sched_cpu_activity activity;
	uint32_t                activity_value;

	state = sched_state_for_cpu(cpu);
	if (state == NULL) return;

	(void)__atomic_fetch_add(&state->stats.total_ticks, 1u, __ATOMIC_RELAXED);
	activity_value = __atomic_load_n(&state->activity, __ATOMIC_ACQUIRE);
	activity =
		activity_value <= SCHED_CPU_ACTIVITY_IDLE ? (enum sched_cpu_activity)activity_value : SCHED_CPU_ACTIVITY_KERNEL;
	switch (activity) {
	case SCHED_CPU_ACTIVITY_THREAD:
		(void)__atomic_fetch_add(&state->stats.thread_ticks, 1u, __ATOMIC_RELAXED);
		break;
	case SCHED_CPU_ACTIVITY_IDLE:
		(void)__atomic_fetch_add(&state->stats.idle_ticks, 1u, __ATOMIC_RELAXED);
		break;
	case SCHED_CPU_ACTIVITY_KERNEL:
	default:
		(void)__atomic_fetch_add(&state->stats.kernel_ticks, 1u, __ATOMIC_RELAXED);
		break;
	}
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

static bool sched_wait_queue_remove_locked(struct thread_wait_queue* queue, struct thread* thread) {
	struct thread* previous = NULL;
	struct thread* current;

	if (queue == NULL || thread == NULL) return false;

	current = queue->head;
	while (current != NULL && current != thread) {
		previous = current;
		current  = current->wait_queue_next;
	}

	if (current == NULL) return false;

	if (previous == NULL) {
		queue->head = current->wait_queue_next;
	}
	else {
		previous->wait_queue_next = current->wait_queue_next;
	}

	if (queue->tail == current) queue->tail = previous;
	current->wait_queue_next = NULL;
	if (queue->depth != 0u) queue->depth--;
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

static void sched_sleep_queue_insert_locked(struct thread* thread, uint64_t deadline_tick) {
	struct thread* cursor;

	if (thread == NULL) return;

	thread->wake_deadline_tick = deadline_tick;
	thread->sleep_queue_next   = NULL;

	if (sched_sleep_head == NULL || sched_sleep_head->wake_deadline_tick > deadline_tick) {
		thread->sleep_queue_next = sched_sleep_head;
		sched_sleep_head         = thread;
		return;
	}

	cursor = sched_sleep_head;
	while (cursor->sleep_queue_next != NULL && cursor->sleep_queue_next->wake_deadline_tick <= deadline_tick) {
		cursor = cursor->sleep_queue_next;
	}

	thread->sleep_queue_next = cursor->sleep_queue_next;
	cursor->sleep_queue_next = thread;
}

static bool sched_sleep_queue_remove_locked(struct thread* thread) {
	struct thread* previous = NULL;
	struct thread* current;

	if (thread == NULL) return false;

	current = sched_sleep_head;
	while (current != NULL && current != thread) {
		previous = current;
		current  = current->sleep_queue_next;
	}

	if (current == NULL) return false;

	if (previous == NULL) {
		sched_sleep_head = current->sleep_queue_next;
	}
	else {
		previous->sleep_queue_next = current->sleep_queue_next;
	}

	current->sleep_queue_next = NULL;
	return true;
}

static enum thread_wait_status sched_thread_wait_status_load(const struct thread* thread) {
	if (thread == NULL) return THREAD_WAIT_STATUS_NONE;
	return (enum thread_wait_status)__atomic_load_n(&thread->wait_status, __ATOMIC_ACQUIRE);
}

static bool sched_thread_wait_status_transition(struct thread* thread, enum thread_wait_status expected,
                                                enum thread_wait_status desired) {
	uint32_t expected_value = (uint32_t)expected;

	if (thread == NULL) return false;

	return __atomic_compare_exchange_n(
		&thread->wait_status, &expected_value, (uint32_t)desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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
	struct sched_cpu_state* state;
	struct thread*          next;
	struct thread*          previous;

	if (cpu == NULL) return;

	state    = sched_state_for_cpu(cpu);
	previous = cpu->current_thread;
	next     = sched_select_next(cpu);
	if (next == NULL) return;

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	if (previous == next) {
		sched_set_current(cpu, next);
		return;
	}

	sched_set_current(cpu, next);
	if (previous != NULL) {
		sched_stat_increment_pair(&sched_stats.context_switch_count,
		                          state == NULL ? NULL : &state->stats.context_switch_count);
		hal_cpu_context_switch(&previous->context, &next->context);
	}
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
	sched_sleep_head = NULL;
	sched_ticks      = 0u;
	sched_stats      = (struct sched_stats){0};

	for (size_t i = 0; i < cpu_total; i++) {
		struct cpu*             cpu = cpu_by_index(i);
		struct sched_cpu_state* state;

		if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return false;

		state          = &sched_cpu_state[cpu->index];
		state->present = true;
		run_queue_init(&state->run_queue);
		sprintf(state->idle_name, "idle/%zu", cpu->index);
		thread_init_idle(&state->idle_thread, cpu, state->idle_name);
		state->activity     = SCHED_CPU_ACTIVITY_KERNEL;
		state->stats        = (struct sched_cpu_stats){0};
		cpu->current_thread = NULL;
		sched_clear_reschedule_request(cpu);
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
	sched_set_cpu_activity(cpu, sched_activity_for_thread(thread));
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
	struct sched_cpu_state* state;
	struct cpu*             cpu = cpu_current();
	struct thread*          current;

	if (cpu == NULL) return;

	state = sched_state_for_cpu(cpu);
	sched_stat_increment_pair(&sched_stats.yield_count, state == NULL ? NULL : &state->stats.yield_count);
	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);

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

void sched_request_reschedule(struct cpu* cpu) {
	if (sched_state_for_cpu(cpu) == NULL) return;

	__atomic_store_n(&cpu->reschedule_requested, true, __ATOMIC_RELEASE);
}

bool sched_reschedule_pending(const struct cpu* cpu) {
	if (sched_state_for_cpu(cpu) == NULL) return false;

	return __atomic_load_n(&cpu->reschedule_requested, __ATOMIC_ACQUIRE);
}

bool sched_handle_interrupt_exit(void) {
	struct sched_cpu_state* state;
	struct cpu*             cpu = cpu_current();
	struct thread*          current;

	if (!sched_reschedule_pending(cpu)) return false;

	state   = sched_state_for_cpu(cpu);
	current = cpu->current_thread;
	sched_clear_reschedule_request(cpu);
	if (current == NULL || thread_is_idle(current) || thread_is_terminated(current) ||
	    sched_run_queue_depth(cpu) == 0u) {
		return false;
	}

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	if (!sched_make_runnable_on_cpu(cpu, current, true)) return false;

	sched_stat_increment_pair(&sched_stats.timeslice_preempt_count,
	                          state == NULL ? NULL : &state->stats.timeslice_preempt_count);
	sched_dispatch_next(cpu);
	return true;
}

uint64_t sched_tick_count(void) {
	struct irq_state state;
	uint64_t         tick_count;

	state      = spinlock_lock_irqsave(&sched_sleep_lock);
	tick_count = sched_ticks;
	spinlock_unlock_irqrestore(&sched_sleep_lock, state);
	return tick_count;
}

bool sched_sleep_until_tick(uint64_t deadline_tick) {
	struct cpu*      cpu = cpu_current();
	struct thread*   current;
	struct irq_state state;

	if (cpu == NULL) return false;

	current = cpu->current_thread;
	if (current == NULL || thread_is_idle(current) || thread_is_terminated(current)) return false;

	state = spinlock_lock_irqsave(&sched_sleep_lock);
	if (deadline_tick <= sched_ticks) {
		spinlock_unlock_irqrestore(&sched_sleep_lock, state);
		return true;
	}

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	thread_mark_blocked(current, THREAD_BLOCK_SLEEP);
	sched_sleep_queue_insert_locked(current, deadline_tick);
	spinlock_unlock_irqrestore(&sched_sleep_lock, state);

	sched_dispatch_next(cpu);
	return true;
}

void sched_tick(void) {
	struct thread*   due = NULL;
	struct irq_state state;
	uint64_t         now;

	sched_account_cpu_tick(cpu_current());
	state = spinlock_lock_irqsave(&sched_sleep_lock);
	sched_ticks++;
	now = sched_ticks;
	spinlock_unlock_irqrestore(&sched_sleep_lock, state);

	for (;;) {
		state = spinlock_lock_irqsave(&sched_sleep_lock);
		if (sched_sleep_head != NULL && sched_sleep_head->wake_deadline_tick <= now) {
			due                   = sched_sleep_head;
			sched_sleep_head      = due->sleep_queue_next;
			due->sleep_queue_next = NULL;
		}
		else {
			due = NULL;
		}
		spinlock_unlock_irqrestore(&sched_sleep_lock, state);

		if (due == NULL) break;
		if (due->blocked_queue != NULL && sched_thread_wait_status_load(due) == THREAD_WAIT_STATUS_PENDING) {
			struct thread_wait_queue* queue = due->blocked_queue;

			if (!sched_thread_wait_status_transition(due, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_TIMED_OUT)) {
				continue;
			}

			state = spinlock_lock_irqsave(&queue->lock);
			(void)sched_wait_queue_remove_locked(queue, due);
			spinlock_unlock_irqrestore(&queue->lock, state);
		}

		(void)sched_make_runnable(due);
	}

	sched_charge_current_timeslice(cpu_current());
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

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	thread_mark_blocked(current, reason);
	queued = sched_wait_queue_enqueue_locked(queue, current);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued) {
		thread_mark_running(current, cpu);
		sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
		return false;
	}

	sched_dispatch_next(cpu);
	return true;
}

bool sched_block_current_until_locked(struct thread_wait_queue* queue, enum thread_block_reason reason,
                                      uint64_t deadline_tick, struct irq_state queue_irq_state) {
	struct cpu*             cpu = cpu_current();
	struct thread*          current;
	enum thread_wait_status wait_status;
	bool                    queued;

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

	spinlock_lock(&sched_sleep_lock);
	if (deadline_tick <= sched_ticks) {
		spinlock_unlock(&sched_sleep_lock);
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return false;
	}

	if (reason == THREAD_BLOCK_NONE) reason = THREAD_BLOCK_WAIT_QUEUE;

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	thread_mark_blocked(current, reason);
	current->blocked_queue = queue;
	__atomic_store_n(&current->wait_status, THREAD_WAIT_STATUS_PENDING, __ATOMIC_RELEASE);
	queued = sched_wait_queue_enqueue_locked(queue, current);
	if (queued) sched_sleep_queue_insert_locked(current, deadline_tick);
	spinlock_unlock(&sched_sleep_lock);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued) {
		thread_mark_running(current, cpu);
		sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
		return false;
	}

	sched_dispatch_next(cpu);
	wait_status = sched_thread_wait_status_load(current);
	__atomic_store_n(&current->wait_status, THREAD_WAIT_STATUS_NONE, __ATOMIC_RELEASE);
	return wait_status == THREAD_WAIT_STATUS_SIGNALED;
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

	for (;;) {
		irq_state = spinlock_lock_irqsave(&queue->lock);
		thread    = sched_wait_queue_dequeue_locked(queue);
		spinlock_unlock_irqrestore(&queue->lock, irq_state);

		if (thread == NULL) return false;

		if (sched_thread_wait_status_load(thread) == THREAD_WAIT_STATUS_PENDING) {
			struct irq_state sleep_state;

			if (!sched_thread_wait_status_transition(thread, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_SIGNALED)) {
				continue;
			}

			sleep_state = spinlock_lock_irqsave(&sched_sleep_lock);
			(void)sched_sleep_queue_remove_locked(thread);
			spinlock_unlock_irqrestore(&sched_sleep_lock, sleep_state);
		}
		else if (thread->blocked_queue == queue) {
			continue;
		}

		return sched_make_runnable(thread);
	}
}

size_t sched_wake_all(struct thread_wait_queue* queue) {
	size_t woken = 0u;

	if (queue == NULL) return 0u;

	while (sched_wake_one(queue)) {
		woken++;
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

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
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

void sched_get_stats(struct sched_stats* out_stats) {
	if (out_stats == NULL) return;

	out_stats->context_switch_count    = __atomic_load_n(&sched_stats.context_switch_count, __ATOMIC_RELAXED);
	out_stats->timeslice_preempt_count = __atomic_load_n(&sched_stats.timeslice_preempt_count, __ATOMIC_RELAXED);
	out_stats->yield_count             = __atomic_load_n(&sched_stats.yield_count, __ATOMIC_RELAXED);
}

bool sched_get_cpu_stats(const struct cpu* cpu, struct sched_cpu_stats* out_stats) {
	const struct sched_cpu_state* state = sched_state_for_cpu(cpu);

	if (state == NULL || out_stats == NULL) return false;

	out_stats->total_ticks             = __atomic_load_n(&state->stats.total_ticks, __ATOMIC_RELAXED);
	out_stats->thread_ticks            = __atomic_load_n(&state->stats.thread_ticks, __ATOMIC_RELAXED);
	out_stats->idle_ticks              = __atomic_load_n(&state->stats.idle_ticks, __ATOMIC_RELAXED);
	out_stats->kernel_ticks            = __atomic_load_n(&state->stats.kernel_ticks, __ATOMIC_RELAXED);
	out_stats->context_switch_count    = __atomic_load_n(&state->stats.context_switch_count, __ATOMIC_RELAXED);
	out_stats->timeslice_preempt_count = __atomic_load_n(&state->stats.timeslice_preempt_count, __ATOMIC_RELAXED);
	out_stats->yield_count             = __atomic_load_n(&state->stats.yield_count, __ATOMIC_RELAXED);
	return true;
}

void sched_debug_dump(void) {
	struct sched_stats stats;

	sched_get_stats(&stats);
	printf("kernel: sched stats: switches=%llu preempts=%llu yields=%llu ticks=%llu\n",
	       (unsigned long long)stats.context_switch_count,
	       (unsigned long long)stats.timeslice_preempt_count,
	       (unsigned long long)stats.yield_count,
	       (unsigned long long)sched_tick_count());
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
