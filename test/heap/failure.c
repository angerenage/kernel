#include <limits.h>

#include "test_support.h"

Test(heap, impossible_allocation_does_not_consume_growth_arenas) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	size_t                        before_total;
	size_t                        before_free;
	void*                         ptr;

	init_test_heap(arena, sizeof(arena));
	before_total = heap_total_bytes();
	before_free  = heap_free_bytes();

	/*
	 * This size can survive malloc's header/alignment arithmetic on 64-bit,
	 * but adding arena sentinels in grow_heap must still be overflow-checked.
	 */
	ptr = malloc(SIZE_MAX - 64u);
	cr_assert_null(ptr, "impossible allocation must fail");
	cr_assert_eq(heap_total_bytes(), before_total, "overflowing growth calculation must not attach useless arenas");
	cr_assert_eq(heap_free_bytes(), before_free, "failed impossible allocation must leave free capacity unchanged");
}
