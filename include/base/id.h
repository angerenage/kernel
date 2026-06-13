#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	atomic_uint64_t next;
} id_allocator_t;

/* Initialize the allocator. Starts allocating from 1 (0 reserved as NULL). */
static inline void id_allocator_init(id_allocator_t* a) {
	atomic_init(&a->next, 1ULL);
}

/* Allocate a single ID. */
static inline uint64_t id_alloc(id_allocator_t* a) {
	return atomic_fetch_add_explicit(&a->next, 1ULL, memory_order_relaxed);
}

/* Allocate a contiguous range of IDs, returns the first ID in the range. */
static inline uint64_t id_alloc_range(id_allocator_t* a, size_t count) {
	return atomic_fetch_add_explicit(&a->next, (uint64_t)count, memory_order_relaxed);
}
