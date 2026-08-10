#include "test_support.h"

Test(heap, allocates_frees_and_reuses_blocks) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	void*                         a;
	void*                         b;
	void*                         reused;

	init_test_heap(arena, sizeof(arena));

	cr_assert_gt(heap_total_bytes(), 0, "heap reported no capacity");
	cr_assert_eq(heap_total_bytes(), heap_free_bytes(), "heap should start fully free");

	a = malloc(24);
	b = malloc(80);

	cr_assert_not_null(a, "malloc returned NULL");
	cr_assert_not_null(b, "malloc returned NULL");
	cr_assert(is_heap_aligned(a), "malloc returned misaligned pointer");
	cr_assert(is_heap_aligned(b), "malloc returned misaligned pointer");
	cr_assert_neq(a, b, "distinct allocations returned the same pointer");
	cr_assert_lt(heap_free_bytes(), heap_total_bytes(), "free space did not decrease");

	free(a);
	reused = malloc(24);

	cr_assert_eq(reused, a, "allocator did not reuse the freed block");
}
