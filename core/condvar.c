#include <core/condvar.h>
#include <core/math.h>
#include <core/sched.h>
#include <hal/clock.h>

static __attribute__((noreturn))
void condvar_trap(void) {
	__builtin_trap();
}

static bool condvar_timeout_deadline(uint64_t timeout_ms, uint64_t* deadline_tick) {
	uint32_t timer_hz;
	uint64_t sleep_ticks;

	if (deadline_tick == NULL) return false;
	if (timeout_ms == 0u) return false;

	timer_hz = hal_clock_frequency();
	if (timer_hz == 0u) return false;

	if (mul_overflow_u64(timeout_ms, (uint64_t)timer_hz, &sleep_ticks) ||
	    add_overflow_u64(sleep_ticks, 999u, &sleep_ticks)) {
		sleep_ticks = UINT64_MAX;
	}
	else {
		sleep_ticks /= 1000u;
	}
	if (sleep_ticks == 0u) sleep_ticks = 1u;
	return !add_overflow_u64(sched_tick_count(), sleep_ticks, deadline_tick);
}

void condvar_init(struct condvar* condvar) {
	if (condvar == NULL) return;

	thread_wait_queue_init(&condvar->waiters);
}

void condvar_wait(struct condvar* condvar, struct mutex* mutex) {
	struct irq_state mutex_state;
	struct irq_state wait_state;
	struct thread*   current;

	if (condvar == NULL || mutex == NULL) return;
	current = sched_current_thread();
	if (current == NULL) condvar_trap();

	mutex_state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner != current) {
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		condvar_trap();
	}

	/* Hold the condvar queue lock across the mutex release to avoid missed wakeups. */
	wait_state   = spinlock_lock_irqsave(&condvar->waiters.lock);
	mutex->owner = NULL;
	spinlock_unlock(&mutex->lock);
	(void)sched_wake_one(&mutex->waiters);
	if (!sched_block_current_locked(&condvar->waiters, THREAD_BLOCK_WAIT_QUEUE, wait_state)) {
		irq_restore(mutex_state);
		condvar_trap();
	}
	irq_restore(mutex_state);
	mutex_lock(mutex);
}

bool condvar_timed_wait(struct condvar* condvar, struct mutex* mutex, uint64_t timeout_ms) {
	struct irq_state mutex_state;
	struct irq_state wait_state;
	uint64_t         deadline_tick;
	bool             signaled;
	struct thread*   current;

	if (condvar == NULL || mutex == NULL) return false;
	current = sched_current_thread();
	if (current == NULL) condvar_trap();

	if (!condvar_timeout_deadline(timeout_ms, &deadline_tick)) return false;

	mutex_state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner != current) {
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		condvar_trap();
	}

	/* Hold the condvar queue lock across the mutex release to avoid missed wakeups. */
	wait_state   = spinlock_lock_irqsave(&condvar->waiters.lock);
	mutex->owner = NULL;
	spinlock_unlock(&mutex->lock);
	(void)sched_wake_one(&mutex->waiters);
	signaled = sched_block_current_until_locked(&condvar->waiters, THREAD_BLOCK_WAIT_QUEUE, deadline_tick, wait_state);
	irq_restore(mutex_state);
	mutex_lock(mutex);
	return signaled;
}

bool condvar_signal(struct condvar* condvar) {
	if (condvar == NULL) return false;

	return sched_wake_one(&condvar->waiters);
}

size_t condvar_broadcast(struct condvar* condvar) {
	if (condvar == NULL) return 0u;

	return sched_wake_all(&condvar->waiters);
}
