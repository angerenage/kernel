#include <base/time.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <core/kthread.h>
#include <core/mutex.h>
#include <core/sched.h>

#define MUTEX_INHERITANCE_CHAIN_LIMIT 64u

void mutex_init(struct mutex* mutex) {
	if (mutex == NULL) return;

	spinlock_init_class(&mutex->lock, "mutex_lock", SPINLOCK_ORDER_MUTEX, SPINLOCK_FLAG_IRQSAVE);
	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELEASE);
	mutex->owner_next = NULL;
	thread_wait_queue_init(&mutex->waiters);
}

static void mutex_recompute_inherited_priority(struct thread* owner) {
	int32_t priority;
	struct mutex* owned;

	if (owner == NULL) return;

	priority = owner->base_priority;
	owned = owner->owned_mutexes;
	while (owned != NULL) {
		struct irq_state wait_state;
		struct thread* waiter;

		wait_state = spinlock_lock_irqsave(&owned->waiters.lock);
		waiter = owned->waiters.head;
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

	__atomic_store_n(&mutex->owner, owner, __ATOMIC_RELEASE);
	mutex->owner_next = owner->owned_mutexes;
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

	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELEASE);
	mutex->owner_next = NULL;
	mutex_recompute_inherited_priority(owner);
}

static bool mutex_chain_contains(struct mutex* const* visited, size_t count, const struct mutex* mutex) {
	for (size_t i = 0u; i < count; i++) {
		if (visited[i] == mutex) return true;
	}
	return false;
}

static struct mutex* mutex_inherit_owner_priority_locked(
		struct mutex* mutex, const struct thread* waiter, struct thread** observed_owner) {
	struct thread* owner;

	if (observed_owner != NULL) *observed_owner = NULL;
	if (mutex == NULL || waiter == NULL) return NULL;

	owner = __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE);
	if (observed_owner != NULL) *observed_owner = owner;
	if (owner == NULL || owner == waiter) return NULL;
	if (owner->effective_priority < waiter->effective_priority) {
		sched_set_thread_effective_priority(owner, waiter->effective_priority);
	}
	return __atomic_load_n(&owner->waiting_mutex, __ATOMIC_ACQUIRE);
}

static void mutex_inherit_waiter_priority_chain(
		struct mutex* origin, struct mutex* current, const struct thread* waiter) {
	struct mutex* visited[MUTEX_INHERITANCE_CHAIN_LIMIT];
	size_t count = 0u;

	if (origin == NULL || waiter == NULL) return;

	visited[count++] = origin;
	while (current != NULL && count < MUTEX_INHERITANCE_CHAIN_LIMIT) {
		struct irq_state state;
		struct thread* owner;
		struct mutex* next;

		if (mutex_chain_contains(visited, count, current)) return;
		visited[count++] = current;
		state = spinlock_lock_irqsave(&current->lock);
		next = mutex_inherit_owner_priority_locked(current, waiter, &owner);
		spinlock_unlock_irqrestore(&current->lock, state);
		if (owner == NULL || owner == waiter) return;
		current = next;
	}
}

static void mutex_recompute_owner_priority(struct mutex* mutex) {
	struct mutex* visited[MUTEX_INHERITANCE_CHAIN_LIMIT];
	struct mutex* current;
	size_t count = 0u;

	if (mutex == NULL) return;

	current = mutex;
	while (current != NULL && count < MUTEX_INHERITANCE_CHAIN_LIMIT) {
		struct irq_state state;
		struct thread* owner;
		struct mutex* next = NULL;

		if (mutex_chain_contains(visited, count, current)) return;
		visited[count++] = current;
		state = spinlock_lock_irqsave(&current->lock);
		owner = __atomic_load_n(&current->owner, __ATOMIC_ACQUIRE);
		if (owner != NULL) {
			mutex_recompute_inherited_priority(owner);
			next = __atomic_load_n(&owner->waiting_mutex, __ATOMIC_ACQUIRE);
		}
		spinlock_unlock_irqrestore(&current->lock, state);
		current = next;
	}
}

bool mutex_release_locked(struct mutex* mutex, struct thread* owner) {
	if (mutex == NULL || owner == NULL || __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != owner) return false;

	mutex_remove_owned_locked(mutex, owner);
	return true;
}

void mutex_abandon_owned(struct thread* owner) {
	while (owner != NULL && owner->owned_mutexes != NULL) {
		struct mutex* mutex = owner->owned_mutexes;
		struct irq_state state = spinlock_lock_irqsave(&mutex->lock);
		bool released = mutex_release_locked(mutex, owner);

		spinlock_unlock_irqrestore(&mutex->lock, state);
		if (!released) hcf();
		(void)sched_wake_one(&mutex->waiters);
	}
}

bool mutex_try_lock(struct mutex* mutex) {
	struct irq_state state;
	struct thread* current;
	bool acquired = false;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&mutex->lock);
	if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == NULL) {
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
	if (current == NULL) hcf();
	kthread_testcancel();

	for (;;) {
		struct irq_state mutex_state;

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == NULL) {
			__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
			mutex_add_owned_locked(mutex, current);
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return;
		}
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			hcf();
		}

		struct thread* observed_owner;
		struct mutex* next;
		struct irq_state wait_state;

		__atomic_store_n(&current->waiting_mutex, mutex, __ATOMIC_RELEASE);
		next = mutex_inherit_owner_priority_locked(mutex, current, &observed_owner);
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		mutex_inherit_waiter_priority_chain(mutex, next, current);

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != observed_owner) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			continue;
		}
		wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);
		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, wait_state)) {
			__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
			irq_restore(mutex_state);
			hcf();
		}
		__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
		irq_restore(mutex_state);
		mutex_recompute_owner_priority(mutex);
		kthread_testcancel();
	}
}

bool mutex_timed_lock(struct mutex* mutex, uint64_t timeout_ms) {
	struct thread* current;
	uint64_t deadline_tick;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) hcf();
	kthread_testcancel();

	if (timeout_ms == 0u) {
		struct irq_state state;

		state = spinlock_lock_irqsave(&mutex->lock);
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == NULL) {
			mutex_add_owned_locked(mutex, current);
			spinlock_unlock_irqrestore(&mutex->lock, state);
			return true;
		}
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == current) {
			spinlock_unlock_irqrestore(&mutex->lock, state);
			hcf();
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
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == NULL) {
			__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
			mutex_add_owned_locked(mutex, current);
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			return true;
		}
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == current) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			hcf();
		}

		struct thread* observed_owner;
		struct mutex* next;

		__atomic_store_n(&current->waiting_mutex, mutex, __ATOMIC_RELEASE);
		next = mutex_inherit_owner_priority_locked(mutex, current, &observed_owner);
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		mutex_inherit_waiter_priority_chain(mutex, next, current);

		mutex_state = spinlock_lock_irqsave(&mutex->lock);
		if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != observed_owner) {
			spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
			continue;
		}
		wait_state = spinlock_lock_irqsave(&mutex->waiters.lock);
		spinlock_unlock(&mutex->lock);
		if (!sched_block_current_until_locked(&mutex->waiters, THREAD_BLOCK_MUTEX, deadline_tick, wait_state)) {
			__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
			irq_restore(mutex_state);
			mutex_recompute_owner_priority(mutex);
			kthread_testcancel();
			return false;
		}
		__atomic_store_n(&current->waiting_mutex, NULL, __ATOMIC_RELEASE);
		irq_restore(mutex_state);
		mutex_recompute_owner_priority(mutex);
		kthread_testcancel();
	}
}

bool mutex_unlock(struct mutex* mutex) {
	struct irq_state state;
	struct thread* current;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	if (current == NULL) return false;

	state = spinlock_lock_irqsave(&mutex->lock);
	if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != current) {
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
	bool locked;

	if (mutex == NULL) return false;

	lock = (struct spinlock*)&mutex->lock;
	state = spinlock_lock_irqsave(lock);
	locked = __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) != NULL;
	spinlock_unlock_irqrestore(lock, state);
	return locked;
}

bool mutex_is_owned_by_current(const struct mutex* mutex) {
	struct irq_state state;
	struct thread* current;
	struct spinlock* lock;
	bool owned;

	if (mutex == NULL) return false;

	current = sched_current_thread();
	lock = (struct spinlock*)&mutex->lock;
	state = spinlock_lock_irqsave(lock);
	owned = current != NULL && __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) == current;
	spinlock_unlock_irqrestore(lock, state);
	return owned;
}

struct thread* mutex_owner(const struct mutex* mutex) {
	struct irq_state state;
	struct spinlock* lock;
	struct thread* owner;

	if (mutex == NULL) return NULL;

	lock = (struct spinlock*)&mutex->lock;
	state = spinlock_lock_irqsave(lock);
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE);
	spinlock_unlock_irqrestore(lock, state);
	return owner;
}

size_t mutex_waiter_count(struct mutex* mutex) {
	if (mutex == NULL) return 0u;

	return thread_wait_queue_depth(&mutex->waiters);
}
