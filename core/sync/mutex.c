#include <base/time.h>
#include <core/kthread.h>
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
	mutex->owner      = NULL;
	mutex->owner_next = NULL;
	thread_wait_queue_init(&mutex->waiters);
}

static void mutex_recompute_inherited_priority(struct thread* owner) {
	int32_t       priority;
	struct mutex* owned;

	if (owner == NULL) return;

	priority = owner->base_priority;
	owned    = owner->owned_mutexes;
	while (owned != NULL) {
		struct irq_state wait_state;
		struct thread*   waiter;

		wait_state = spinlock_lock_irqsave(&owned->waiters.lock);
		waiter     = owned->waiters.head;
		while (waiter != NULL) {
			if (waiter->effective_priority > priority) priority = waiter->effective_priority;
			waiter = waiter->wait_queue_next;
		}
		spinlock_unlock_irqrestore(&owned->waiters.lock, wait_state);

		owned = owned->owner_next;
	}

	sched_set_thread_effective_priority(owner, priority);
}

static void mutex_add_owned_locked(struct mutex* mutex, struct thread* owner) {
	if (mutex == NULL || owner == NULL) return;

	mutex->owner         = owner;
	mutex->owner_next    = owner->owned_mutexes;
	owner->owned_mutexes = mutex;
	mutex_recompute_inherited_priority(owner);
}

static void mutex_remove_owned_locked(struct mutex* mutex, struct thread* owner) {
	struct mutex** cursor;

	if (mutex == NULL || owner == NULL) return;

	cursor = &owner->owned_mutexes;
	while (*cursor != NULL && *cursor != mutex) {
		cursor = &(*cursor)->owner_next;
	}
	if (*cursor == mutex) *cursor = mutex->owner_next;

	mutex->owner      = NULL;
	mutex->owner_next = NULL;
	mutex_recompute_inherited_priority(owner);
}

static void mutex_inherit_waiter_priority_locked(struct mutex* mutex, const struct thread* waiter) {
	struct thread* owner;

	if (mutex == NULL || waiter == NULL) return;

	owner = mutex->owner;
	if (owner == NULL || owner == waiter) return;
	if (waiter->effective_priority <= owner->effective_priority) return;

	sched_set_thread_effective_priority(owner, waiter->effective_priority);
}

static void mutex_recompute_owner_priority(struct mutex* mutex) {
	struct irq_state state;
	struct thread*   owner;

	if (mutex == NULL) return;

	state = spinlock_lock_irqsave(&mutex->lock);
	owner = mutex->owner;
	if (owner != NULL) mutex_recompute_inherited_priority(owner);
	spinlock_unlock_irqrestore(&mutex->lock, state);
}

bool mutex_release_locked(struct mutex* mutex, struct thread* owner) {
	if (mutex == NULL || owner == NULL || mutex->owner != owner) return false;

	mutex_remove_owned_locked(mutex, owner);
	return true;
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
		mutex_add_owned_locked(mutex, current);
		acquired = true;
	}
	spinlock_unlock_irqrestore(&mutex->lock, state);
	return acquired;
}

void mutex_lock(struct mutex* mutex) {
	struct thread* current;

	if (mutex == NULL) return;

	current = sched_current_thread();
	if (current == NULL) mutex_trap();
	kthread_testcancel();

	for (;;) {
		struct irq_state mutex_state;

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex_add_owned_locked(mutex, current);
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return;
		}
		if (mutex->owner == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			mutex_trap();
		}

		struct irq_state wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);

		mutex_inherit_waiter_priority_locked(mutex, current);
		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, wait_state)) {
			irq_restore(mutex_state);
			mutex_trap();
		}
		irq_restore(mutex_state);
		mutex_recompute_owner_priority(mutex);
		kthread_testcancel();
	}
}

bool mutex_timed_lock(struct mutex* mutex, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t       deadline_tick;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) mutex_trap();
	kthread_testcancel();

	if (timeout_ms == 0u) {
		struct irq_state state;

		state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex_add_owned_locked(mutex, current);
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

	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

	for (;;) {
		struct irq_state mutex_state;
		struct irq_state wait_state;

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (mutex->owner == NULL) {
			mutex_add_owned_locked(mutex, current);
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return true;
		}
		if (mutex->owner == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			mutex_trap();
		}

		wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);
		mutex_inherit_waiter_priority_locked(mutex, current);
		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_until_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, deadline_tick, wait_state)) {
			irq_restore(mutex_state);
			mutex_recompute_owner_priority(mutex);
			kthread_testcancel();
			return false;
		}
		irq_restore(mutex_state);
		kthread_testcancel();
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

	(void)mutex_release_locked(mutex, current);
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
