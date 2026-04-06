#pragma once

#include <stdbool.h>
#include <stdint.h>

struct spinlock {
	uint32_t state;
};

#define SPINLOCK_INIT                                                                                                  \
	{                                                                                                                  \
		.state = 0u,                                                                                                   \
	}

static inline void spinlock_init(struct spinlock* lock) {
	lock->state = 0u;
}

static inline void spinlock_relax(void) {
#if defined(PLATFORM_PC_X86_64)
	__asm__ volatile("pause");
#elif defined(PLATFORM_PC_AARCH64)
	__asm__ volatile("yield");
#else
	__asm__ volatile("" ::: "memory");
#endif
}

static inline bool spinlock_try_lock(struct spinlock* lock) {
	return __atomic_exchange_n(&lock->state, 1u, __ATOMIC_ACQUIRE) == 0u;
}

static inline void spinlock_lock(struct spinlock* lock) {
	while (!spinlock_try_lock(lock)) {
		while (__atomic_load_n(&lock->state, __ATOMIC_RELAXED) != 0u) {
			spinlock_relax();
		}
	}
}

static inline void spinlock_unlock(struct spinlock* lock) {
	__atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}
