#pragma once

#include <stdint.h>

typedef uint64_t atomic_uint64_t;

typedef enum {
	memory_order_relaxed = 0,
} memory_order;

static inline void atomic_init(atomic_uint64_t* obj, uint64_t value) {
	__atomic_store_n(obj, value, __ATOMIC_RELAXED);
}

static inline uint64_t atomic_fetch_add_explicit(atomic_uint64_t* obj, uint64_t arg, memory_order order) {
	int builtin_order = __ATOMIC_RELAXED;

	switch (order) {
	case memory_order_relaxed:
	default:
		builtin_order = __ATOMIC_RELAXED;
		break;
	}

	return __atomic_fetch_add(obj, arg, builtin_order);
}
