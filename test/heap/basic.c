#include <libc/string.h>

#include "heap_test.h"

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

Test(heap, strdup_allocates_kernel_copy) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	char*                         copy;

	init_test_heap(arena, sizeof(arena));

	copy = strdup("owned-name");
	cr_assert_not_null(copy, "strdup returned NULL");
	cr_assert_str_eq(copy, "owned-name");
	cr_assert_neq(copy, "owned-name", "strdup should return a distinct allocation");

	copy[0] = 'O';
	cr_assert_str_eq(copy, "Owned-name");

	free(copy);
}

Test(heap, strndup_limits_source_scan_and_terminates_copy) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	const char                    source[] = {'a', 'b', 'c', 'd'};
	char*                         copy;

	init_test_heap(arena, sizeof(arena));

	copy = strndup(source, 3u);
	cr_assert_not_null(copy, "strndup returned NULL");
	cr_assert_str_eq(copy, "abc");

	free(copy);
}
