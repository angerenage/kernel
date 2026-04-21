#include <base/time.h>
#include <core/kthread.h>
#include <core/rwlock.h>
#include <core/sched.h>
#include <hal/clock.h>

static __attribute__((noreturn))
void rwlock_trap(void) {
	__builtin_trap();
}

static bool rwlock_can_grant_read_locked(const struct rwlock* rwlock) {
	return rwlock->writer == NULL && rwlock->waiting_writers == 0u;
}

static bool rwlock_can_grant_write_locked(const struct rwlock* rwlock) {
	return rwlock->writer == NULL && rwlock->reader_count == 0u;
}

static bool rwlock_should_wake_readers_locked(const struct rwlock* rwlock) {
	return rwlock->writer == NULL && rwlock->waiting_writers == 0u;
}

void rwlock_init(struct rwlock* rwlock) {
	if (rwlock == NULL) return;

	spinlock_init_class(&rwlock->lock, "rwlock_lock", SPINLOCK_ORDER_MUTEX, SPINLOCK_FLAG_IRQSAVE);
	rwlock->reader_count    = 0u;
	rwlock->waiting_writers = 0u;
	rwlock->writer          = NULL;
	thread_wait_queue_init(&rwlock->readers);
	thread_wait_queue_init(&rwlock->writers);
}

bool rwlock_try_read_lock(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread*   current;
	bool             acquired = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	state   = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->writer != current && rwlock_can_grant_read_locked(rwlock)) {
		rwlock->reader_count++;
		acquired = true;
	}
	spinlock_unlock_irqrestore(&rwlock->lock, state);
	return acquired;
}

void rwlock_read_lock(struct rwlock* rwlock) {
	struct thread* current;

	if (rwlock == NULL) return;

	current = sched_current_thread();
	if (current == NULL) rwlock_trap();
	kthread_testcancel();

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;

		rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
		if (rwlock_can_grant_read_locked(rwlock)) {
			rwlock->reader_count++;
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			rwlock_trap();
		}

		wait_state = spinlock_lock_irqsave(&rwlock->readers.lock);
		spinlock_unlock(&rwlock->lock);
		if (!sched_block_current_locked(&rwlock->readers, THREAD_BLOCK_RWLOCK, wait_state)) {
			irq_restore(rwlock_state);
			rwlock_trap();
		}
		irq_restore(rwlock_state);
		kthread_testcancel();
	}
}

bool rwlock_timed_read_lock(struct rwlock* rwlock, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t       deadline_tick;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) rwlock_trap();
	kthread_testcancel();

	if (timeout_ms == 0u) return rwlock_try_read_lock(rwlock);
	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;

		rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
		if (rwlock_can_grant_read_locked(rwlock)) {
			rwlock->reader_count++;
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return true;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			rwlock_trap();
		}

		wait_state = spinlock_lock_irqsave(&rwlock->readers.lock);
		spinlock_unlock(&rwlock->lock);
		if (!sched_block_current_until_locked(&rwlock->readers, THREAD_BLOCK_RWLOCK, deadline_tick, wait_state)) {
			irq_restore(rwlock_state);
			kthread_testcancel();
			return false;
		}
		irq_restore(rwlock_state);
		kthread_testcancel();
	}
}

bool rwlock_try_write_lock(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread*   current;
	bool             acquired = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock_can_grant_write_locked(rwlock)) {
		rwlock->writer = current;
		acquired       = true;
	}
	spinlock_unlock_irqrestore(&rwlock->lock, state);
	return acquired;
}

void rwlock_write_lock(struct rwlock* rwlock) {
	struct thread* current;
	bool           counted_waiter = false;

	if (rwlock == NULL) return;

	current = sched_current_thread();
	if (current == NULL) rwlock_trap();
	kthread_testcancel();

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;
		bool             wake_readers = false;

		rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
		if (counted_waiter && thread_should_cancel(current)) {
			if (rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			wake_readers = rwlock_should_wake_readers_locked(rwlock);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			if (wake_readers) (void)sched_wake_all(&rwlock->readers);
			kthread_testcancel();
		}
		if (rwlock_can_grant_write_locked(rwlock)) {
			if (counted_waiter && rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			rwlock->writer = current;
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			rwlock_trap();
		}
		if (!counted_waiter) {
			rwlock->waiting_writers++;
			counted_waiter = true;
		}

		wait_state = spinlock_lock_irqsave(&rwlock->writers.lock);
		spinlock_unlock(&rwlock->lock);
		if (!sched_block_current_locked(&rwlock->writers, THREAD_BLOCK_RWLOCK, wait_state)) {
			irq_restore(rwlock_state);

			rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
			if (counted_waiter && rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			wake_readers = rwlock_should_wake_readers_locked(rwlock);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			if (wake_readers) (void)sched_wake_all(&rwlock->readers);
			rwlock_trap();
		}
		irq_restore(rwlock_state);
	}
}

bool rwlock_timed_write_lock(struct rwlock* rwlock, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t       deadline_tick;
	bool           counted_waiter = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) rwlock_trap();
	kthread_testcancel();

	if (timeout_ms == 0u) return rwlock_try_write_lock(rwlock);
	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;
		bool             wake_readers = false;

		rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
		if (counted_waiter && thread_should_cancel(current)) {
			if (rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			wake_readers = rwlock_should_wake_readers_locked(rwlock);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			if (wake_readers) (void)sched_wake_all(&rwlock->readers);
			kthread_testcancel();
		}
		if (rwlock_can_grant_write_locked(rwlock)) {
			if (counted_waiter && rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			rwlock->writer = current;
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return true;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			rwlock_trap();
		}
		if (!counted_waiter) {
			rwlock->waiting_writers++;
			counted_waiter = true;
		}

		wait_state = spinlock_lock_irqsave(&rwlock->writers.lock);
		spinlock_unlock(&rwlock->lock);
		if (!sched_block_current_until_locked(&rwlock->writers, THREAD_BLOCK_RWLOCK, deadline_tick, wait_state)) {
			irq_restore(rwlock_state);

			rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
			if (counted_waiter && rwlock->waiting_writers != 0u) rwlock->waiting_writers--;
			wake_readers = rwlock_should_wake_readers_locked(rwlock);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			if (wake_readers) (void)sched_wake_all(&rwlock->readers);
			kthread_testcancel();
			return false;
		}
		irq_restore(rwlock_state);
	}
}

bool rwlock_read_unlock(struct rwlock* rwlock) {
	struct irq_state state;
	bool             wake_writer = false;

	if (rwlock == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->reader_count == 0u || rwlock->writer != NULL) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}

	rwlock->reader_count--;
	if (rwlock->reader_count == 0u && rwlock->waiting_writers != 0u) wake_writer = true;
	spinlock_unlock_irqrestore(&rwlock->lock, state);

	if (wake_writer) (void)sched_wake_one(&rwlock->writers);
	return true;
}

bool rwlock_write_unlock(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread*   current;
	bool             wake_writer  = false;
	bool             wake_readers = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->writer != current) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}

	rwlock->writer = NULL;
	if (rwlock->waiting_writers != 0u) {
		wake_writer = true;
	}
	else {
		wake_readers = true;
	}
	spinlock_unlock_irqrestore(&rwlock->lock, state);

	if (wake_writer) (void)sched_wake_one(&rwlock->writers);
	if (wake_readers) {
		(void)sched_wake_all(&rwlock->readers);
	}
	return true;
}

size_t rwlock_reader_count(const struct rwlock* rwlock) {
	struct irq_state state;
	struct spinlock* lock;
	size_t           reader_count;

	if (rwlock == NULL) return 0u;

	lock         = (struct spinlock*)&rwlock->lock;
	state        = spinlock_lock_irqsave(lock);
	reader_count = rwlock->reader_count;
	spinlock_unlock_irqrestore(lock, state);
	return reader_count;
}

size_t rwlock_waiter_count(struct rwlock* rwlock) {
	size_t readers;
	size_t writers;

	if (rwlock == NULL) return 0u;

	readers = thread_wait_queue_depth(&rwlock->readers);
	writers = thread_wait_queue_depth(&rwlock->writers);
	return readers + writers;
}
