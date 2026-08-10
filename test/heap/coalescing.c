#include "test_support.h"

Test(heap, freeing_middle_then_neighbors_recovers_one_reusable_extent) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	void*                         a;
	void*                         b;
	void*                         c;
	void*                         merged;
	size_t                        initial_free;

	init_test_heap(arena, sizeof(arena));
	initial_free = heap_free_bytes();

	a = malloc(256u);
	b = malloc(256u);
	c = malloc(256u);
	cr_assert_not_null(a);
	cr_assert_not_null(b);
	cr_assert_not_null(c);

	free(b);
	free(a);
	free(c);
	cr_assert_eq(heap_free_bytes(), initial_free, "freeing adjacent blocks must recover the initial free capacity");

	merged = malloc(768u);
	cr_assert_not_null(merged, "coalesced neighboring blocks must satisfy a request larger than each original block");
	cr_assert_eq(merged, a, "address-ordered coalescing should reuse the beginning of the recovered extent");
}

Test(heap, blocks_from_multiple_arenas_remain_reusable_after_cross_arena_free_order) {
	_Alignas(4096) static uint8_t arena[KiB(128)];
	void*                         blocks[96] = {0};
	size_t                        count      = 0u;
	size_t                        grown_total;

	init_test_heap(arena, sizeof(arena));

	while (count < 96u) {
		blocks[count] = malloc(512u);
		if (blocks[count] == NULL) break;
		count++;
		if (heap_total_bytes() > (4u * 4096u - 2u * heap_sentinel_size)) break;
	}

	cr_assert_gt(count, 1u, "test must allocate enough blocks to trigger heap growth");
	grown_total = heap_total_bytes();
	cr_assert_gt(grown_total, 4u * 4096u - 2u * heap_sentinel_size, "test did not create more than one arena");

	for (size_t i = 0u; i < count; i += 2u) free(blocks[i]);
	for (size_t i = 1u; i < count; i += 2u) free(blocks[i]);

	cr_assert_eq(
		heap_free_bytes(), heap_total_bytes(), "free-list ordering across arenas must preserve every free block");

	for (size_t i = 0u; i < count; i++) {
		void* ptr = malloc(512u);
		cr_assert_not_null(ptr, "all previously released multi-arena blocks must remain reusable");
	}
}
