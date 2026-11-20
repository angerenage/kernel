#include <base/time.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <core/kthread.h>
#include <core/rwlock.h>
#include <core/sched.h>

static struct thread_rwlock_read_hold* rwlock_find_reader_hold(struct thread* owner, struct rwlock* rwlock) {
	if (owner == NULL || rwlock == NULL) return NULL;
	for (size_t i = 0u; i < THREAD_RWLOCK_READ_HOLD_CAPACITY; i++) {
		if (owner->owned_rwlock_reads[i].rwlock == rwlock) return &owner->owned_rwlock_reads[i];
	}
	return NULL;
}

static struct thread_rwlock_read_hold* rwlock_find_free_reader_hold(struct thread* owner) {
	if (owner == NULL) return NULL;
	for (size_t i = 0u; i < THREAD_RWLOCK_READ_HOLD_CAPACITY; i++) {
		if (owner->owned_rwlock_reads[i].rwlock == NULL) return &owner->owned_rwlock_reads[i];
	}
	return NULL;
}

static bool rwlock_add_reader_locked(struct rwlock* rwlock, struct thread* owner) {
	struct thread_rwlock_read_hold* hold = rwlock_find_reader_hold(owner, rwlock);

	if (hold == NULL) hold = rwlock_find_free_reader_hold(owner);
	if (hold == NULL || hold->count == SIZE_MAX || rwlock->reader_count == SIZE_MAX) return false;
	hold->rwlock = rwlock;
	hold->count++;
	rwlock->reader_count++;
	return true;
}

static void rwlock_add_writer_locked(struct rwlock* rwlock, struct thread* owner) {
	if (rwlock == NULL || owner == NULL || rwlock->writer != NULL) hcf();
	rwlock->writer = owner;
	rwlock->owner_next = owner->owned_rwlock_writes;
	owner->owned_rwlock_writes = rwlock;
}

static void rwlock_remove_writer_locked(struct rwlock* rwlock, struct thread* owner) {
	struct rwlock** cursor;

	if (rwlock == NULL || owner == NULL || rwlock->writer != owner) return;
	cursor = &owner->owned_rwlock_writes;
	while (*cursor != NULL && *cursor != rwlock) cursor = &(*cursor)->owner_next;
	if (*cursor != rwlock) hcf();
	*cursor = rwlock->owner_next;
	rwlock->writer = NULL;
	rwlock->owner_next = NULL;
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
	rwlock->reader_count = 0u;
	rwlock->waiting_writers = 0u;
	rwlock->writer = NULL;
	rwlock->owner_next = NULL;
	thread_wait_queue_init(&rwlock->readers);
	thread_wait_queue_init(&rwlock->writers);
}

bool rwlock_try_read_lock(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread* current;
	bool acquired = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;
	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->writer != current && rwlock_can_grant_read_locked(rwlock)) {
		acquired = rwlock_add_reader_locked(rwlock, current);
	}
	spinlock_unlock_irqrestore(&rwlock->lock, state);
	return acquired;
}

void rwlock_read_lock(struct rwlock* rwlock) {
	struct thread* current;

	if (rwlock == NULL) return;

	current = sched_current_thread();
	if (current == NULL) hcf();
	kthread_testcancel();

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;

		rwlock_state = spinlock_lock_irqsave(&rwlock->lock);
		if (rwlock_can_grant_read_locked(rwlock)) {
			if (!rwlock_add_reader_locked(rwlock, current)) {
				spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
				hcf();
			}
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			hcf();
		}

		wait_state = spinlock_lock_irqsave(&rwlock->readers.lock);
		spinlock_unlock(&rwlock->lock);
		if (!sched_block_current_locked(&rwlock->readers, THREAD_BLOCK_RWLOCK, wait_state)) {
			irq_restore(rwlock_state);
			hcf();
		}
		irq_restore(rwlock_state);
		kthread_testcancel();
	}
}

bool rwlock_timed_read_lock(struct rwlock* rwlock, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t deadline_tick;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) hcf();
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
			if (!rwlock_add_reader_locked(rwlock, current)) {
				spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
				return false;
			}
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return true;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			hcf();
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
	struct thread* current;
	bool acquired = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock_can_grant_write_locked(rwlock)) {
		rwlock_add_writer_locked(rwlock, current);
		acquired = true;
	}
	spinlock_unlock_irqrestore(&rwlock->lock, state);
	return acquired;
}

void rwlock_write_lock(struct rwlock* rwlock) {
	struct thread* current;
	bool counted_waiter = false;

	if (rwlock == NULL) return;

	current = sched_current_thread();
	if (current == NULL) hcf();
	kthread_testcancel();

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;
		bool wake_readers = false;

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
			rwlock_add_writer_locked(rwlock, current);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			hcf();
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
			hcf();
		}
		irq_restore(rwlock_state);
	}
}

bool rwlock_timed_write_lock(struct rwlock* rwlock, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t deadline_tick;
	bool counted_waiter = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) hcf();
	kthread_testcancel();

	if (timeout_ms == 0u) return rwlock_try_write_lock(rwlock);
	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

	for (;;) {
		struct irq_state rwlock_state;
		struct irq_state wait_state;
		bool wake_readers = false;

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
			rwlock_add_writer_locked(rwlock, current);
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			return true;
		}
		if (rwlock->writer == current) {
			spinlock_unlock_irqrestore(&rwlock->lock, rwlock_state);
			hcf();
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
	struct thread_rwlock_read_hold* hold;
	struct irq_state state;
	struct thread* current;
	bool wake_writer = false;

	if (rwlock == NULL) return false;
	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	hold = rwlock_find_reader_hold(current, rwlock);
	if (hold == NULL || hold->count == 0u || rwlock->reader_count == 0u || rwlock->writer != NULL) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}

	hold->count--;
	rwlock->reader_count--;
	if (hold->count == 0u) hold->rwlock = NULL;
	if (rwlock->reader_count == 0u && rwlock->waiting_writers != 0u) wake_writer = true;
	spinlock_unlock_irqrestore(&rwlock->lock, state);

	if (wake_writer) (void)sched_wake_one(&rwlock->writers);
	return true;
}

bool rwlock_write_unlock(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread* current;
	bool wake_writer = false;
	bool wake_readers = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->writer != current) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}

	rwlock_remove_writer_locked(rwlock, current);
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

bool rwlock_downgrade(struct rwlock* rwlock) {
	struct irq_state state;
	struct thread* current;
	bool wake_readers = false;

	if (rwlock == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&rwlock->lock);
	if (rwlock->writer != current) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}
	if (rwlock_find_reader_hold(current, rwlock) == NULL && rwlock_find_free_reader_hold(current) == NULL) {
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		return false;
	}

	rwlock_remove_writer_locked(rwlock, current);
	if (!rwlock_add_reader_locked(rwlock, current)) hcf();
	wake_readers = rwlock_should_wake_readers_locked(rwlock);
	spinlock_unlock_irqrestore(&rwlock->lock, state);

	if (wake_readers) (void)sched_wake_all(&rwlock->readers);
	return true;
}

void rwlock_abandon_owned(struct thread* owner) {
	while (owner != NULL && owner->owned_rwlock_writes != NULL) {
		struct rwlock* rwlock = owner->owned_rwlock_writes;
		struct irq_state state = spinlock_lock_irqsave(&rwlock->lock);
		bool wake_writer;

		if (rwlock->writer != owner) hcf();
		rwlock_remove_writer_locked(rwlock, owner);
		wake_writer = rwlock->waiting_writers != 0u;
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		if (wake_writer) (void)sched_wake_one(&rwlock->writers);
		else (void)sched_wake_all(&rwlock->readers);
	}

	if (owner == NULL) return;
	for (size_t i = 0u; i < THREAD_RWLOCK_READ_HOLD_CAPACITY; i++) {
		struct thread_rwlock_read_hold* hold = &owner->owned_rwlock_reads[i];
		struct rwlock* rwlock = hold->rwlock;
		struct irq_state state;
		bool wake_writer;

		if (rwlock == NULL) continue;
		state = spinlock_lock_irqsave(&rwlock->lock);
		if (hold->count == 0u || hold->count > rwlock->reader_count) hcf();
		rwlock->reader_count -= hold->count;
		hold->rwlock = NULL;
		hold->count = 0u;
		wake_writer = rwlock->reader_count == 0u && rwlock->waiting_writers != 0u;
		spinlock_unlock_irqrestore(&rwlock->lock, state);
		if (wake_writer) (void)sched_wake_one(&rwlock->writers);
	}
}

size_t rwlock_reader_count(const struct rwlock* rwlock) {
	struct irq_state state;
	struct spinlock* lock;
	size_t reader_count;

	if (rwlock == NULL) return 0u;

	lock = (struct spinlock*)&rwlock->lock;
	state = spinlock_lock_irqsave(lock);
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
