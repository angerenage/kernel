#pragma once

#include <pthread.h>
#include <stdint.h>

struct test_barrier {
	uint32_t arrived;
	uint32_t total;
	uint32_t generation;
};

static inline void test_relax(void) {
	__asm__ volatile("" ::: "memory");
}

static inline void test_barrier_init(struct test_barrier* barrier, uint32_t total) {
	barrier->arrived    = 0u;
	barrier->total      = total;
	barrier->generation = 0u;
}

static inline void test_barrier_wait(struct test_barrier* barrier) {
	uint32_t generation = __atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE);
	uint32_t arrived    = __atomic_add_fetch(&barrier->arrived, 1u, __ATOMIC_ACQ_REL);

	if (arrived == barrier->total) {
		__atomic_store_n(&barrier->arrived, 0u, __ATOMIC_RELEASE);
		__atomic_add_fetch(&barrier->generation, 1u, __ATOMIC_ACQ_REL);
		return;
	}

	while (__atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE) == generation) {
		test_relax();
	}
}
