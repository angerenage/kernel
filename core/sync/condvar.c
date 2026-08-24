#include <base/time.h>
#include <core/condvar.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <hal/clock.h>
#include <hal/hcf.h>

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
	if (current == NULL) hcf();
	kthread_testcancel();

	mutex_state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner != current) {
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		hcf();
	}

	/* Hold the condvar queue lock across the mutex release to avoid missed wakeups. */
	wait_state = spinlock_lock_irqsave(&condvar->waiters.lock);
	if (!mutex_release_locked(mutex, current)) {
		spinlock_unlock_irqrestore(&condvar->waiters.lock, wait_state);
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		hcf();
	}
	spinlock_unlock(&mutex->lock);
	(void)sched_wake_one(&mutex->waiters);
	if (!sched_block_current_locked(&condvar->waiters, THREAD_BLOCK_WAIT_QUEUE, wait_state)) {
		irq_restore(mutex_state);
		hcf();
	}
	irq_restore(mutex_state);
	mutex_lock(mutex);
	kthread_testcancel();
}

bool condvar_timed_wait(struct condvar* condvar, struct mutex* mutex, uint64_t timeout_ms) {
	struct irq_state mutex_state;
	struct irq_state wait_state;
	uint64_t         deadline_tick;
	bool             signaled;
	struct thread*   current;

	if (condvar == NULL || mutex == NULL) return false;
	current = sched_current_thread();
	if (current == NULL) hcf();
	kthread_testcancel();

	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick))
		return false;

	mutex_state = spinlock_lock_irqsave(&mutex->lock);
	if (mutex->owner != current) {
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		hcf();
	}

	/* Hold the condvar queue lock across the mutex release to avoid missed wakeups. */
	wait_state = spinlock_lock_irqsave(&condvar->waiters.lock);
	if (!mutex_release_locked(mutex, current)) {
		spinlock_unlock_irqrestore(&condvar->waiters.lock, wait_state);
		spinlock_unlock_irqrestore(&mutex->lock, mutex_state);
		hcf();
	}
	spinlock_unlock(&mutex->lock);
	(void)sched_wake_one(&mutex->waiters);
	signaled = sched_block_current_until_locked(&condvar->waiters, THREAD_BLOCK_WAIT_QUEUE, deadline_tick, wait_state);
	irq_restore(mutex_state);
	mutex_lock(mutex);
	kthread_testcancel();
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
