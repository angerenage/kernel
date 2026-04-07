#pragma once

#include <stdint.h>

enum spinlock_order {
	SPINLOCK_ORDER_NONE         = 0u,
	SPINLOCK_ORDER_CPU_TOPOLOGY = 10u,
	SPINLOCK_ORDER_SCHED        = 15u,
	SPINLOCK_ORDER_CLOCK        = 20u,
	SPINLOCK_ORDER_VMM          = 30u,
	SPINLOCK_ORDER_VADDR        = 40u,
	SPINLOCK_ORDER_PAGING       = 50u,
	SPINLOCK_ORDER_PMM          = 60u,
	SPINLOCK_ORDER_KHEAP        = 70u,
};

enum spinlock_flags {
	SPINLOCK_FLAG_NONE            = 0u,
	SPINLOCK_FLAG_IRQSAVE         = 1u << 0,
	SPINLOCK_FLAG_ALLOW_EXCEPTION = 1u << 1,
};
