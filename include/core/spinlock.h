#pragma once

#include <core/lock.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

/* Small busy-wait lock with optional debug metadata for ordering and irq-save validation. */
struct spinlock {
	uint32_t state;
#if CORE_LOCK_DEBUG
	const char* name;
	uint32_t    order;
	uint32_t    flags;
	uint32_t    owner_cpu;
#endif
};

#if CORE_LOCK_DEBUG
#define SPINLOCK_INIT_CLASS(lock_name, lock_order, lock_flags)                                                         \
	{                                                                                                                  \
		.state     = 0u,                                                                                               \
		.name      = lock_name,                                                                                        \
		.order     = (lock_order),                                                                                     \
		.flags     = (lock_flags),                                                                                     \
		.owner_cpu = 0u,                                                                                               \
	}
#else
#define SPINLOCK_INIT_CLASS(lock_name, lock_order, lock_flags)                                                         \
	{                                                                                                                  \
		.state = 0u,                                                                                                   \
	}
#endif

#define SPINLOCK_INIT SPINLOCK_INIT_CLASS(NULL, SPINLOCK_ORDER_NONE, SPINLOCK_FLAG_NONE)

/* Result of the debug-only pre-acquire validation pass. */
enum spinlock_debug_check {
	SPINLOCK_DEBUG_CHECK_OK = 0,
	SPINLOCK_DEBUG_CHECK_REENTRANT,
	SPINLOCK_DEBUG_CHECK_ORDER,
	SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED,
	SPINLOCK_DEBUG_CHECK_EXCEPTION_DISALLOWED,
};

/* Reset a lock to the unlocked state with no debug classification. */
void spinlock_init(struct spinlock* lock);

/* Reset a lock and attach debug metadata describing its class and acquisition requirements. */
void spinlock_init_class(struct spinlock* lock, const char* name, uint32_t order, uint32_t flags);

/* Architecture-specific pause/yield hint used while spinning. */
void spinlock_relax(void);

/* Run the debug-only acquisition checks without actually taking the lock. */
enum spinlock_debug_check spinlock_debug_check_acquire(const struct spinlock* lock);

/* Attempt to take the lock once, returning false immediately if it is already held. */
bool spinlock_try_lock(struct spinlock* lock);

/* Spin until the lock becomes available, then acquire it. */
void spinlock_lock(struct spinlock* lock);

/* Release the lock. */
void spinlock_unlock(struct spinlock* lock);

/* Disable interrupts, then acquire the lock and return the previous interrupt state. */
struct irq_state spinlock_lock_irqsave(struct spinlock* lock);

/* Release the lock and restore the interrupt state captured by spinlock_lock_irqsave(). */
void spinlock_unlock_irqrestore(struct spinlock* lock, struct irq_state state);
