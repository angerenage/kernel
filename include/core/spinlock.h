#pragma once

#include <core/lock.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

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

enum spinlock_debug_check {
	SPINLOCK_DEBUG_CHECK_OK = 0,
	SPINLOCK_DEBUG_CHECK_REENTRANT,
	SPINLOCK_DEBUG_CHECK_ORDER,
	SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED,
	SPINLOCK_DEBUG_CHECK_EXCEPTION_DISALLOWED,
};

void                      spinlock_init(struct spinlock* lock);
void                      spinlock_init_class(struct spinlock* lock, const char* name, uint32_t order, uint32_t flags);
void                      spinlock_relax(void);
enum spinlock_debug_check spinlock_debug_check_acquire(const struct spinlock* lock);
bool                      spinlock_try_lock(struct spinlock* lock);
void                      spinlock_lock(struct spinlock* lock);
void                      spinlock_unlock(struct spinlock* lock);
struct irq_state          spinlock_lock_irqsave(struct spinlock* lock);
void                      spinlock_unlock_irqrestore(struct spinlock* lock, struct irq_state state);
