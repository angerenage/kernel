#include <core/math.h>
#include <core/mutex.h>
#include <core/sched.h>
#include <hal/clock.h>

static __attribute__((noreturn))
void mutex_trap(void) {
	__builtin_trap();
}

void mutex_init(struct mutex* mutex) {
	if (mutex == NULL) return;

	spinlock_init_class(&mutex->lock, "mutex_lock", SPINLOCK_ORDER_MUTEX, SPINLOCK_FLAG_IRQSAVE);
	mutex->owner = NULL;
	thread_wait_queue_init(&mutex->waiters);
}

bool mutex_try_lock(struct mutex* mutex) {
	struct irq_state state;
	struct thread*   current;
	bool             acquired = false;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner == NULL) {
		mutex->owner = current;
		acquired     = true;
	}
	spinlock_unlock_irqrestore(&mutex->lock, state);
	return acquired;
}

void mutex_lock(struct mutex* mutex) {
	struct thread* current;

	if (mutex == NULL) return;

	current = sched_current_thread();
	if (current == NULL) mutex_trap();

	for (;;) {
		struct irq_state mutex_state;

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex->owner = current;
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return;
		}
		if (mutex->owner == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			mutex_trap();
		}

		struct irq_state wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);

		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, wait_state)) {
			irq_restore(mutex_state);
			mutex_trap();
		}
		irq_restore(mutex_state);
	}
}

bool mutex_timed_lock(struct mutex* mutex, uint64_t timeout_ms) {
	struct thread* current;
	uint32_t       timer_hz;
	uint64_t       sleep_ticks;
	uint64_t       deadline_tick;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) mutex_trap();

	if (timeout_ms == 0u) {
		struct irq_state state;

		state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex->owner = current;
			spinlock_unlock_irqrestore(&mutex->lock, state);
			return true;
		}
		if (mutex->owner == current) {
			spinlock_unlock_irqrestore(&mutex->lock, state);
			mutex_trap();
		}
		spinlock_unlock_irqrestore(&mutex->lock, state);
		return false;
	}

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
	if (add_overflow_u64(sched_tick_count(), sleep_ticks, &deadline_tick)) return false;

	for (;;) {
		struct irq_state mutex_state;
		struct irq_state wait_state;

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex->owner = current;
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return true;
		}
		if (mutex->owner == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			mutex_trap();
		}

		wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);
		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_until_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, deadline_tick, wait_state)) {
			irq_restore(mutex_state);
			return false;
		}
		irq_restore(mutex_state);
	}
}

bool mutex_unlock(struct mutex* mutex) {
	struct irq_state state;
	struct thread*   current;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner != current) {
		spinlock_unlock_irqrestore(&mutex->lock, state);
		return false;
	}

	mutex->owner = NULL;
	spinlock_unlock_irqrestore(&mutex->lock, state);
	(void)sched_wake_one(&mutex->waiters);
	return true;
}

bool mutex_is_locked(const struct mutex* mutex) {
	struct irq_state state;
	struct spinlock* lock;
	bool             locked;

	if (mutex == NULL) return false;

	lock   = (struct spinlock*)&mutex->lock;
	state  = spinlock_lock_irqsave(lock);
	locked = mutex->owner != NULL;
	spinlock_unlock_irqrestore(lock, state);
	return locked;
}

bool mutex_is_owned_by_current(const struct mutex* mutex) {
	struct irq_state state;
	struct thread*   current;
	struct spinlock* lock;
	bool             owned;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	lock    = (struct spinlock*)&mutex->lock;
	state   = spinlock_lock_irqsave(lock);
	owned   = current != NULL && mutex->owner == current;
	spinlock_unlock_irqrestore(lock, state);
	return owned;
}

struct thread* mutex_owner(const struct mutex* mutex) {
	struct irq_state state;
	struct spinlock* lock;
	struct thread*   owner;

	if (mutex == NULL) return NULL;

	lock  = (struct spinlock*)&mutex->lock;
	state = spinlock_lock_irqsave(lock);
	owner = mutex->owner;
	spinlock_unlock_irqrestore(lock, state);
	return owner;
}

size_t mutex_waiter_count(struct mutex* mutex) {
	if (mutex == NULL) return 0u;

	return thread_wait_queue_depth(&mutex->waiters);
}
