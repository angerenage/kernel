#include <core/cpu.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <hal/cpu.h>
#include <stdint.h>
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
static struct thread* sched_sleep_head;
static uint64_t       sched_ticks;

static struct sched_cpu_state* sched_state_for_cpu(const struct cpu* cpu);

static void sched_stat_increment(uint64_t* counter) {
	if (counter == NULL) return;
	(void)__atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

static void sched_clear_reschedule_request(struct cpu* cpu) {
	if (cpu == NULL) return;

	__atomic_store_n(&cpu->reschedule_requested, false, __ATOMIC_RELEASE);
}

static int32_t sched_effective_priority(const struct thread* thread) {
	return thread == NULL ? THREAD_PRIORITY_MIN : thread->effective_priority;
}

static bool sched_run_queue_has_priority_at_least(struct cpu* cpu, int32_t priority) {
	struct sched_cpu_state* state;
	struct irq_state        irq_state;
	struct thread*          head;
	bool                    has_priority;

	state = sched_state_for_cpu(cpu);
	if (state == NULL) return false;

	irq_state    = spinlock_lock_irqsave(&state->run_queue.lock);
	head         = state->run_queue.head;
	has_priority = head != NULL && sched_effective_priority(head) >= priority;
	spinlock_unlock_irqrestore(&state->run_queue.lock, irq_state);
	return has_priority;
}

static bool sched_thread_should_preempt_current(struct cpu* cpu, const struct thread* thread) {
	const struct thread* current;

	if (cpu == NULL || thread == NULL) return false;

	current = cpu->current_thread;
	if (current == NULL || thread_is_idle(current) || thread_is_terminated(current)) return false;

	return sched_effective_priority(thread) > sched_effective_priority(current);
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
	if (!sched_run_queue_has_priority_at_least(cpu, sched_effective_priority(current))) return;

	sched_request_reschedule(cpu);
	if (cpu != cpu_current()) hal_cpu_kick(cpu);
}

static struct sched_cpu_state* sched_state_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return NULL;
	if (!sched_cpu_state[cpu->index].present) return NULL;
	return &sched_cpu_state[cpu->index];
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

static bool sched_activate_thread_address_space(const struct thread* previous, const struct thread* next) {
	struct address_space* previous_space;
	struct address_space* next_space;

	previous_space = previous == NULL ? NULL : previous->address_space;
	next_space     = next == NULL ? NULL : next->address_space;
	if (previous_space == NULL && next_space == NULL) return true;
	if (previous_space == next_space) return true;

	if (next_space == NULL) next_space = address_space_kernel();
	if (!address_space_is_initialized(next_space)) return false;
	return address_space_activate(next_space);
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

static bool sched_cpu_can_accept_balanced_work(const struct cpu* cpu) {
	return sched_state_for_cpu(cpu) != NULL && cpu != NULL && cpu->current_thread != NULL;
}

static size_t sched_cpu_effective_load(const struct cpu* cpu) {
	size_t               load = 0u;
	const struct thread* current;

	if (!sched_cpu_can_accept_balanced_work(cpu)) return SIZE_MAX;

	load    = sched_run_queue_depth((struct cpu*)cpu);
	current = cpu->current_thread;
	if (current != NULL && !thread_is_idle(current) && !thread_is_terminated(current)) {
		load++;
	}

	return load;
}

static struct cpu* sched_pick_least_loaded_cpu(const struct thread* thread) {
	struct cpu* best_cpu  = NULL;
	size_t      best_load = 0u;

	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);
		size_t      load;

		if (!sched_cpu_can_accept_balanced_work(cpu)) continue;

		load = sched_cpu_effective_load(cpu);
		if (best_cpu == NULL || load < best_load) {
			best_cpu  = cpu;
			best_load = load;
		}
	}

	if (best_cpu == NULL) return NULL;

	if (sched_cpu_can_accept_balanced_work(thread == NULL ? NULL : thread->cpu) &&
	    sched_cpu_effective_load(thread->cpu) == best_load) {
		return thread->cpu;
	}

	if (sched_cpu_can_accept_balanced_work(cpu_current()) && sched_cpu_effective_load(cpu_current()) == best_load) {
		return cpu_current();
	}

	if (sched_cpu_can_accept_balanced_work(cpu_bsp()) && sched_cpu_effective_load(cpu_bsp()) == best_load) {
		return cpu_bsp();
	}

	return best_cpu;
}

static struct cpu* sched_target_cpu_for_thread(const struct thread* thread) {
	struct cpu* cpu;

	if (thread == NULL) return NULL;

	if (sched_state_for_cpu(thread->preferred_cpu) != NULL) return thread->preferred_cpu;

	cpu = sched_pick_least_loaded_cpu(thread);
	if (cpu != NULL) return cpu;

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
	(void)__atomic_fetch_and(&current->flags, ~(uint32_t)THREAD_FLAG_QUEUED, __ATOMIC_ACQ_REL);
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

static void sched_thread_wait_status_store(struct thread* thread, enum thread_wait_status status) {
	if (thread == NULL) return;

	__atomic_store_n(&thread->wait_status, (uint32_t)status, __ATOMIC_RELEASE);
}

static bool sched_abort_cancelled_block_locked(struct thread_wait_queue* queue, struct thread* current,
                                               struct cpu* cpu) {
	if (!thread_should_cancel(current)) return false;

	(void)sched_thread_wait_status_transition(current, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_CANCELED);
	(void)sched_wait_queue_remove_locked(queue, current);
	thread_mark_running(current, cpu);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
	sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
	return true;
}

static bool sched_abort_interrupted_block_locked(struct thread_wait_queue* queue, struct thread* current,
                                                 struct cpu* cpu) {
	if (!thread_interrupt_pending(current)) return false;
	if (!sched_thread_wait_status_transition(current, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_INTERRUPTED)) {
		return false;
	}

	(void)sched_wait_queue_remove_locked(queue, current);
	thread_mark_running(current, cpu);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
	sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
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
	struct sched_cpu_state* state;
	struct thread*          next;
	struct thread*          previous;

	if (cpu == NULL) return;

	sched_finish_context_switch();
	state    = sched_state_for_cpu(cpu);
	previous = cpu->current_thread;
	next     = sched_select_next(cpu);
	if (next == NULL) return;
	if (!sched_activate_thread_address_space(previous, next)) return;

	if (previous == next) {
		sched_set_current(cpu, next);
		return;
	}

	if (previous != NULL && previous->state == THREAD_STATE_EXITING) {
		__atomic_store_n(&cpu->context_switch_in_progress, true, __ATOMIC_RELEASE);
		__atomic_store_n(&cpu->deferred_reap_thread, previous, __ATOMIC_RELEASE);
	}

	sched_set_current(cpu, next);
	if (previous != NULL) {
		sched_stat_increment(state == NULL ? NULL : &state->stats.context_switch_count);
		hal_cpu_context_switch(&previous->context, &next->context);
		sched_complete_context_switch();
	}
}

static bool sched_make_runnable_on_cpu(struct cpu* cpu, struct thread* thread, bool allow_current) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	bool                    queued;

	if (state == NULL || thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return false;
	if (thread_is_queued(thread)) return false;
	if (!allow_current && cpu != NULL && cpu->current_thread == thread) return false;

	thread_mark_ready(thread, cpu);
	queued = run_queue_enqueue(&state->run_queue, thread);
	if (!queued) return false;

	if (cpu != NULL && cpu != cpu_current()) {
		sched_request_reschedule(cpu);
		hal_cpu_kick(cpu);
	}
	else if (sched_thread_should_preempt_current(cpu, thread)) {
		sched_request_reschedule(cpu);
	}

	return true;
}

static bool sched_make_waiter_runnable(struct thread* thread) {
	struct cpu* cpu;

	if (thread == NULL) return false;
	cpu = thread->cpu;
	if (cpu != NULL && cpu->current_thread == thread) return sched_make_runnable_on_cpu(cpu, thread, true);
	return sched_make_runnable(thread);
}

bool sched_init(void) {
	size_t cpu_total = cpu_count();

	if (cpu_total == 0u || cpu_total > SCHED_MAX_CPU_COUNT) return false;

	for (size_t i = 0; i < SCHED_MAX_CPU_COUNT; i++) {
		sched_cpu_state[i] = (struct sched_cpu_state){0};
	}
	sched_sleep_head = NULL;
	sched_ticks      = 0u;

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
	cpu->kernel_entry_stack_top =
		thread != NULL && thread->kernel_stack_top != 0u ? thread->kernel_stack_top : cpu->boot_stack_top;
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

void sched_set_thread_effective_priority(struct thread* thread, int32_t priority) {
	struct sched_cpu_state* state;
	struct cpu*             cpu;
	int32_t                 normalized_priority;

	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread)) return;

	normalized_priority = thread_priority_clamp(priority);
	if (thread->effective_priority == normalized_priority) return;

	thread->effective_priority = normalized_priority;
	cpu                        = thread->cpu;
	state                      = sched_state_for_cpu(cpu);

	if (state != NULL && thread_is_queued(thread)) {
		(void)run_queue_requeue(&state->run_queue, thread);
		if (cpu != NULL && cpu != cpu_current()) {
			sched_request_reschedule(cpu);
			hal_cpu_kick(cpu);
		}
		return;
	}

	if (state != NULL && cpu != NULL && cpu->current_thread == thread &&
	    sched_run_queue_has_priority_at_least(cpu, normalized_priority + 1)) {
		sched_request_reschedule(cpu);
		if (cpu != cpu_current()) hal_cpu_kick(cpu);
	}
}

void sched_yield(void) {
	struct sched_cpu_state* state;
	struct cpu*             cpu = cpu_current();
	struct thread*          current;

	if (cpu == NULL) return;

	state = sched_state_for_cpu(cpu);
	sched_stat_increment(state == NULL ? NULL : &state->stats.yield_count);
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

	if (cpu == NULL || __atomic_load_n(&cpu->context_switch_in_progress, __ATOMIC_ACQUIRE)) return false;
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

	sched_stat_increment(state == NULL ? NULL : &state->stats.timeslice_preempt_count);
	sched_dispatch_next(cpu);
	return true;
}

void sched_finish_context_switch(void) {
	struct cpu*    cpu = cpu_current();
	struct thread* thread;
	bool           joinable;

	if (cpu == NULL || __atomic_load_n(&cpu->context_switch_in_progress, __ATOMIC_ACQUIRE)) return;

	thread = __atomic_exchange_n(&cpu->deferred_reap_thread, NULL, __ATOMIC_ACQ_REL);
	if (thread == NULL) return;
	if (thread == cpu->current_thread) {
		__atomic_store_n(&cpu->deferred_reap_thread, thread, __ATOMIC_RELEASE);
		return;
	}
	if (thread->state != THREAD_STATE_EXITING) return;

	joinable = thread_is_joinable(thread);
	thread_mark_zombie(thread);
	if (thread->reap_callback != NULL) thread_notify_reap(thread);
	if (joinable) (void)sched_wake_all(&thread->join_wait_queue);
}

void sched_complete_context_switch(void) {
	struct cpu* cpu = cpu_current();

	if (cpu == NULL) return;
	__atomic_store_n(&cpu->context_switch_in_progress, false, __ATOMIC_RELEASE);
	sched_finish_context_switch();
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
	struct cpu*             cpu = cpu_current();
	struct thread*          current;
	struct irq_state        state;
	enum thread_wait_status wait_status;

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
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_PENDING);
	sched_sleep_queue_insert_locked(current, deadline_tick);
	spinlock_unlock_irqrestore(&sched_sleep_lock, state);

	sched_dispatch_next(cpu);
	wait_status = sched_thread_wait_status_load(current);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
	(void)wait_status;
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

		{
			enum thread_wait_status wait_status = sched_thread_wait_status_load(due);
			enum thread_wait_status wake_status =
				due->blocked_queue == NULL ? THREAD_WAIT_STATUS_SIGNALED : THREAD_WAIT_STATUS_TIMED_OUT;

			if (wait_status == THREAD_WAIT_STATUS_PENDING) {
				if (!sched_thread_wait_status_transition(due, THREAD_WAIT_STATUS_PENDING, wake_status)) continue;
			}
			else if (wait_status != THREAD_WAIT_STATUS_NONE || due->state != THREAD_STATE_BLOCKED) {
				continue;
			}
		}
		if (due->blocked_queue != NULL) {
			struct thread_wait_queue* queue = due->blocked_queue;

			state = spinlock_lock_irqsave(&queue->lock);
			(void)sched_wait_queue_remove_locked(queue, due);
			spinlock_unlock_irqrestore(&queue->lock, state);
		}

		(void)sched_make_waiter_runnable(due);
	}

	sched_charge_current_timeslice(cpu_current());
}

void sched_tick_remote(struct cpu* cpu) {
	if (cpu == NULL) return;

	sched_account_cpu_tick(cpu);
	sched_charge_current_timeslice(cpu);
}

bool sched_block_current_locked(struct thread_wait_queue* queue, enum thread_block_reason reason,
                                struct irq_state queue_irq_state) {
	struct cpu*    cpu = cpu_current();
	struct thread* current;
	bool           cancelled = false;
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
	current->blocked_queue = queue;
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_PENDING);
	queued = sched_wait_queue_enqueue_locked(queue, current);
	if (queued) cancelled = sched_abort_cancelled_block_locked(queue, current, cpu);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued || cancelled) {
		if (!cancelled) {
			thread_mark_running(current, cpu);
			sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
			sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
		}
		return false;
	}

	sched_dispatch_next(cpu);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
	return true;
}

enum sched_block_result sched_block_current_interruptible_locked(struct thread_wait_queue* queue,
                                                                 enum thread_block_reason  reason,
                                                                 struct irq_state          queue_irq_state) {
	struct cpu*             cpu = cpu_current();
	struct thread*          current;
	enum thread_wait_status wait_status;
	bool                    cancelled   = false;
	bool                    interrupted = false;
	bool                    queued;

	if (queue == NULL) return SCHED_BLOCK_FAILED;
	if (cpu == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return SCHED_BLOCK_FAILED;
	}

	current = cpu->current_thread;
	if (current == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		(void)sched_start_cpu(cpu);
		return SCHED_BLOCK_FAILED;
	}
	if (thread_is_idle(current) || thread_is_terminated(current)) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return SCHED_BLOCK_FAILED;
	}
	if (thread_interrupt_pending(current)) {
		spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);
		return SCHED_BLOCK_INTERRUPTED;
	}

	if (reason == THREAD_BLOCK_NONE) reason = THREAD_BLOCK_WAIT_QUEUE;

	sched_set_cpu_activity(cpu, SCHED_CPU_ACTIVITY_KERNEL);
	thread_mark_blocked(current, reason);
	current->blocked_queue = queue;
	(void)__atomic_fetch_or(&current->flags, (uint32_t)THREAD_FLAG_WAIT_INTERRUPTIBLE, __ATOMIC_ACQ_REL);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_PENDING);
	queued = sched_wait_queue_enqueue_locked(queue, current);
	if (queued) cancelled = sched_abort_cancelled_block_locked(queue, current, cpu);
	if (queued && !cancelled) interrupted = sched_abort_interrupted_block_locked(queue, current, cpu);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued || cancelled || interrupted) {
		if (!cancelled && !interrupted) {
			thread_mark_running(current, cpu);
			sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
			sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
		}
		return interrupted ? SCHED_BLOCK_INTERRUPTED : SCHED_BLOCK_FAILED;
	}

	sched_dispatch_next(cpu);
	wait_status = sched_thread_wait_status_load(current);
	sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
	if (wait_status == THREAD_WAIT_STATUS_INTERRUPTED) return SCHED_BLOCK_INTERRUPTED;
	if (wait_status == THREAD_WAIT_STATUS_SIGNALED) return SCHED_BLOCK_SIGNALED;
	return SCHED_BLOCK_FAILED;
}

bool sched_block_current_until_locked(struct thread_wait_queue* queue, enum thread_block_reason reason,
                                      uint64_t deadline_tick, struct irq_state queue_irq_state) {
	struct cpu*             cpu = cpu_current();
	struct thread*          current;
	enum thread_wait_status wait_status;
	bool                    cancelled = false;
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
	if (queued) cancelled = sched_abort_cancelled_block_locked(queue, current, cpu);
	if (queued && !cancelled) sched_sleep_queue_insert_locked(current, deadline_tick);
	spinlock_unlock(&sched_sleep_lock);
	spinlock_unlock_irqrestore(&queue->lock, queue_irq_state);

	if (!queued || cancelled) {
		if (!cancelled) {
			thread_mark_running(current, cpu);
			sched_thread_wait_status_store(current, THREAD_WAIT_STATUS_NONE);
			sched_set_cpu_activity(cpu, sched_activity_for_thread(current));
		}
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
		else if (sched_thread_wait_status_load(thread) != THREAD_WAIT_STATUS_NONE) {
			continue;
		}

		if (sched_make_waiter_runnable(thread)) return true;
		if (!thread_is_terminated(thread)) return false;
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

bool sched_interrupt_thread(struct thread* thread) {
	struct thread_wait_queue* queue;
	struct cpu*               cpu;

	if (thread == NULL || thread_is_idle(thread) || thread_is_terminated(thread) || !thread_interrupt_pending(thread)) {
		return false;
	}

	cpu = thread->cpu;
	if (thread->state == THREAD_STATE_RUNNING) {
		if (cpu != NULL && cpu != cpu_current()) {
			sched_request_reschedule(cpu);
			hal_cpu_kick(cpu);
		}
		return true;
	}
	if (thread->state != THREAD_STATE_BLOCKED ||
	    (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) & THREAD_FLAG_WAIT_INTERRUPTIBLE) == 0u) {
		return true;
	}

	queue = thread->blocked_queue;
	if (queue != NULL) {
		struct irq_state queue_state = spinlock_lock_irqsave(&queue->lock);

		if (thread->state != THREAD_STATE_BLOCKED || thread->blocked_queue != queue ||
		    (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) & THREAD_FLAG_WAIT_INTERRUPTIBLE) == 0u ||
		    !sched_thread_wait_status_transition(thread, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_INTERRUPTED)) {
			spinlock_unlock_irqrestore(&queue->lock, queue_state);
			return true;
		}
		(void)sched_wait_queue_remove_locked(queue, thread);
		spinlock_unlock_irqrestore(&queue->lock, queue_state);
		(void)sched_make_waiter_runnable(thread);
	}
	return true;
}

void sched_cancel_thread(struct thread* thread) {
	struct thread_wait_queue* queue;

	if (!thread_should_cancel(thread)) return;

	if (thread->state == THREAD_STATE_RUNNING) {
		if (thread->cpu != NULL) sched_request_reschedule(thread->cpu);
		return;
	}
	if (thread->state != THREAD_STATE_BLOCKED) return;

	queue = thread->blocked_queue;
	if (queue != NULL) {
		struct irq_state queue_state = spinlock_lock_irqsave(&queue->lock);
		bool             remove_sleep;

		if (thread->state != THREAD_STATE_BLOCKED || thread->blocked_queue != queue ||
		    !sched_thread_wait_status_transition(thread, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_CANCELED)) {
			spinlock_unlock_irqrestore(&queue->lock, queue_state);
			return;
		}
		(void)sched_wait_queue_remove_locked(queue, thread);
		remove_sleep = thread->wake_deadline_tick != 0u;
		spinlock_unlock_irqrestore(&queue->lock, queue_state);

		if (remove_sleep) {
			struct irq_state sleep_state = spinlock_lock_irqsave(&sched_sleep_lock);

			(void)sched_sleep_queue_remove_locked(thread);
			spinlock_unlock_irqrestore(&sched_sleep_lock, sleep_state);
		}
		(void)sched_make_waiter_runnable(thread);
		return;
	}

	if (thread->block_reason == THREAD_BLOCK_SLEEP) {
		struct irq_state sleep_state = spinlock_lock_irqsave(&sched_sleep_lock);

		if (thread->state != THREAD_STATE_BLOCKED || thread->blocked_queue != NULL ||
		    thread->block_reason != THREAD_BLOCK_SLEEP ||
		    !sched_thread_wait_status_transition(thread, THREAD_WAIT_STATUS_PENDING, THREAD_WAIT_STATUS_CANCELED)) {
			spinlock_unlock_irqrestore(&sched_sleep_lock, sleep_state);
			return;
		}
		(void)sched_sleep_queue_remove_locked(thread);
		spinlock_unlock_irqrestore(&sched_sleep_lock, sleep_state);
		(void)sched_make_waiter_runnable(thread);
	}
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

	*out_stats = (struct sched_stats){0};
	for (size_t i = 0; i < SCHED_MAX_CPU_COUNT; i++) {
		if (!sched_cpu_state[i].present) continue;

		out_stats->context_switch_count +=
			__atomic_load_n(&sched_cpu_state[i].stats.context_switch_count, __ATOMIC_RELAXED);
		out_stats->timeslice_preempt_count +=
			__atomic_load_n(&sched_cpu_state[i].stats.timeslice_preempt_count, __ATOMIC_RELAXED);
		out_stats->yield_count += __atomic_load_n(&sched_cpu_state[i].stats.yield_count, __ATOMIC_RELAXED);
	}
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

	if (cpu == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}

	if (cpu->current_thread == NULL && !sched_start_cpu(cpu)) {
		for (;;) {
			hal_cpu_park();
		}
	}

	for (;;) {
		if (sched_run_queue_depth(cpu) != 0u) {
			sched_yield();
			continue;
		}

		/*
		 * Some early SMP backends do not yet provide a reliable cross-CPU
		 * wakeup path for parked APs.
		 * Polling here keeps application
		 * processors responsive to remotely enqueued work until their wake

		 * * mechanism is fully wired up.
		 */
		if (cpu != NULL && cpu->role == CPU_ROLE_AP) {
			spinlock_relax();
			continue;
		}

		hal_cpu_park();
	}
}
