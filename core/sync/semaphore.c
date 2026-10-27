#include <base/time.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <core/semaphore.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <kernel/trace.h>

#define TRACE_SEMAPHORE_PHASE(phase, type)                                                                             \
	kernel_trace_emit(KERNEL_TRACE_EVENT_SEMAPHORE_PHASE, type, kernel_trace_thread_id(sched_current_thread()), phase)

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
	if (sched_current_thread() == NULL) hcf();
	kthread_testcancel();
	if (!semaphore_acquire_cancelable(semaphore)) {
		kthread_testcancel();
		hcf();
	}
}

bool semaphore_acquire_cancelable(struct semaphore* semaphore) {
	struct thread* current;

	if (semaphore == NULL) return false;
	current = sched_current_thread();
	if (current == NULL) return false;

	for (;;) {
		struct irq_state semaphore_state;
		struct irq_state wait_state;
		bool             blocked;

		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CHECK, KERNEL_TRACE_RECORD_BEGIN);
		semaphore_state = spinlock_lock_irqsave(&semaphore->lock);
		if (thread_should_cancel(current)) {
			spinlock_unlock_irqrestore(&semaphore->lock, semaphore_state);
			TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CHECK, KERNEL_TRACE_RECORD_END);
			return false;
		}
		if (semaphore->count != 0u) {
			semaphore->count--;
			spinlock_unlock_irqrestore(&semaphore->lock, semaphore_state);
			TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CHECK, KERNEL_TRACE_RECORD_END);
			return true;
		}
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CHECK, KERNEL_TRACE_RECORD_END);

		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_QUEUE_LOCK, KERNEL_TRACE_RECORD_BEGIN);
		wait_state = spinlock_lock_irqsave(&semaphore->waiters.lock);
		spinlock_unlock(&semaphore->lock);
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_QUEUE_LOCK, KERNEL_TRACE_RECORD_END);
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_SCHED_BLOCK, KERNEL_TRACE_RECORD_BEGIN);
		blocked = sched_block_current_locked(&semaphore->waiters, THREAD_BLOCK_SEMAPHORE, wait_state);
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_SCHED_BLOCK, KERNEL_TRACE_RECORD_END);
		if (!blocked) {
			irq_restore(semaphore_state);
			return false;
		}
		irq_restore(semaphore_state);
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CANCEL_CHECK, KERNEL_TRACE_RECORD_BEGIN);
		blocked = !thread_should_cancel(current);
		TRACE_SEMAPHORE_PHASE(KERNEL_TRACE_SEMAPHORE_CANCEL_CHECK, KERNEL_TRACE_RECORD_END);
		if (!blocked) return false;
	}
}

bool semaphore_timed_acquire(struct semaphore* semaphore, uint64_t timeout_ms) {
	uint64_t deadline_tick;

	if (semaphore == NULL) return false;
	if (sched_current_thread() == NULL) hcf();
	kthread_testcancel();

	if (timeout_ms == 0u) return semaphore_try_acquire(semaphore);
	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

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
			kthread_testcancel();
			return false;
		}
		irq_restore(semaphore_state);
		kthread_testcancel();
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
