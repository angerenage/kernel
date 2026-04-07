#include "kheap_test.h"

#include <core/pmm.h>

static uint8_t* grow_base;
static size_t   grow_capacity;
static size_t   grow_offset;

static bool test_grow(size_t page_count, void** out_base) {
	size_t bytes  = page_count * PMM_PAGE_SIZE;
	size_t offset = 0;

	if (out_base) *out_base = NULL;
	if (!out_base) return false;

	for (;;) {
		offset = __atomic_load_n(&grow_offset, __ATOMIC_ACQUIRE);
		if (bytes > grow_capacity - offset) return false;
		if (__atomic_compare_exchange_n(
				&grow_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = grow_base + offset;
			return true;
		}
	}
}

void init_test_kheap(uint8_t* arena, size_t arena_size) {
	cr_assert_eq(((uintptr_t)arena & (PMM_PAGE_SIZE - 1u)), 0, "test arena must be page-aligned");
	cr_assert_eq((arena_size & (PMM_PAGE_SIZE - 1u)), 0, "test arena size must be page-aligned");

	grow_base     = arena;
	grow_capacity = arena_size;
	__atomic_store_n(&grow_offset, 0u, __ATOMIC_RELEASE);

	cr_assert(kheap_init_with_grower(test_grow), "kheap_init_with_grower failed");
}

bool is_kheap_aligned(const void* ptr) {
	return (((uintptr_t)ptr) & 0x0fu) == 0;
}
