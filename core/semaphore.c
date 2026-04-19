#include <core/math.h>
#include <core/sched.h>
#include <core/semaphore.h>
#include <hal/clock.h>

static __attribute__((noreturn))
void semaphore_trap(void) {
	__builtin_trap();
}

static bool semaphore_timeout_deadline(uint64_t timeout_ms, uint64_t* deadline_tick) {
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

void semaphore_init(struct semaphore* semaphore, size_t initial_count) {
	if (semaphore == NULL) return;

	spinlock_init_class(&semaphore->lock, "semaphore_lock", SPINLOCK_ORDER_MUTEX, SPINLOCK_FLAG_IRQSAVE);
	semaphore->count = initial_count;
	thread_wait_queue_init(&semaphore->waiters);
}

bool semaphore_try_acquire(struct semaphore* semaphore) {
	struct irq_state state;
	bool             acquired = false;

	if (semaphore == NULL) return false;

	state = spinlock_lock_irqsave(&semaphore->lock);
	if (semaphore->count != 0u) {
		semaphore->count--;
		acquired = true;
	}
	spinlock_unlock_irqrestore(&semaphore->lock, state);
	return acquired;
}

void semaphore_acquire(struct semaphore* semaphore) {
	if (semaphore == NULL) return;
	if (sched_current_thread() == NULL) semaphore_trap();

	for (;;) {
		struct irq_state semaphore_state;
		struct irq_state wait_state;

		semaphore_state = spinlock_lock_irqsave(&semaphore->lock);
		if (semaphore->count != 0u) {
			semaphore->count--;
			spinlock_unlock_irqrestore(&semaphore->lock, semaphore_state);
			return;
		}

		wait_state = spinlock_lock_irqsave(&semaphore->waiters.lock);
		spinlock_unlock(&semaphore->lock);
		if (!sched_block_current_locked(&semaphore->waiters, THREAD_BLOCK_SEMAPHORE, wait_state)) {
			irq_restore(semaphore_state);
			semaphore_trap();
		}
		irq_restore(semaphore_state);
	}
}

bool semaphore_timed_acquire(struct semaphore* semaphore, uint64_t timeout_ms) {
	uint64_t deadline_tick;

	if (semaphore == NULL) return false;
	if (sched_current_thread() == NULL) semaphore_trap();

	if (timeout_ms == 0u) return semaphore_try_acquire(semaphore);
	if (!semaphore_timeout_deadline(timeout_ms, &deadline_tick)) return false;

	for (;;) {
		struct irq_state semaphore_state;
		struct irq_state wait_state;

		semaphore_state = spinlock_lock_irqsave(&semaphore->lock);
		if (semaphore->count != 0u) {
			semaphore->count--;
			spinlock_unlock_irqrestore(&semaphore->lock, semaphore_state);
			return true;
		}

		wait_state = spinlock_lock_irqsave(&semaphore->waiters.lock);
		spinlock_unlock(&semaphore->lock);
		if (!sched_block_current_until_locked(&semaphore->waiters, THREAD_BLOCK_SEMAPHORE, deadline_tick, wait_state)) {
			irq_restore(semaphore_state);
			return false;
		}
		irq_restore(semaphore_state);
	}
}

bool semaphore_release(struct semaphore* semaphore) {
	struct irq_state state;

	if (semaphore == NULL) return false;

	state = spinlock_lock_irqsave(&semaphore->lock);
	if (semaphore->count == (size_t)-1) {
		spinlock_unlock_irqrestore(&semaphore->lock, state);
		return false;
	}

	semaphore->count++;
	spinlock_unlock_irqrestore(&semaphore->lock, state);
	(void)sched_wake_one(&semaphore->waiters);
	return true;
}

size_t semaphore_count(const struct semaphore* semaphore) {
	struct irq_state state;
	struct spinlock* lock;
	size_t           count;

	if (semaphore == NULL) return 0u;

	lock  = (struct spinlock*)&semaphore->lock;
	state = spinlock_lock_irqsave(lock);
	count = semaphore->count;
	spinlock_unlock_irqrestore(lock, state);
	return count;
}

size_t semaphore_waiter_count(struct semaphore* semaphore) {
	if (semaphore == NULL) return 0u;

	return thread_wait_queue_depth(&semaphore->waiters);
}
