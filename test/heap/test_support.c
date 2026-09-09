#include "test_support.h"

#include <core/pmm.h>
#include <test_memory.h>

static uint8_t* grow_base;
static size_t   grow_capacity;
static size_t   grow_offset;

bool heap_grow_region(size_t minimum_size, void** out_base, size_t* out_size) {
	size_t bytes  = minimum_size;
	size_t offset = 0;

	if (out_base == NULL || out_size == NULL) return false;
	*out_base = NULL;
	*out_size = 0u;

	for (;;) {
		offset = __atomic_load_n(&grow_offset, __ATOMIC_ACQUIRE);
		if (bytes > grow_capacity - offset) return false;
		if (__atomic_compare_exchange_n(
				&grow_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = grow_base + offset;
			*out_size = bytes;
			return true;
		}
	}
}

void init_test_heap(uint8_t* arena, size_t arena_size) {
	cr_assert_eq(((uintptr_t)arena & (TEST_MAPPING_GRANULE - 1u)), 0, "test arena must be page-aligned");
	cr_assert_eq((arena_size & (TEST_MAPPING_GRANULE - 1u)), 0, "test arena size must be page-aligned");
	grow_base     = arena;
	grow_capacity = arena_size;
	__atomic_store_n(&grow_offset, 0u, __ATOMIC_RELEASE);

	cr_assert(heap_init(), "heap_init failed");
}

bool is_heap_aligned(const void* ptr) {
	return (((uintptr_t)ptr) & 0x0fu) == 0;
}
