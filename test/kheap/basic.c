#include <string.h>

#include "kheap_test.h"

Test(kheap, allocates_frees_and_reuses_blocks) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	void*                         a;
	void*                         b;
	void*                         reused;

	init_test_kheap(arena, sizeof(arena));

	cr_assert_gt(kheap_total_bytes(), 0, "heap reported no capacity");
	cr_assert_eq(kheap_total_bytes(), kheap_free_bytes(), "heap should start fully free");

	a = kmalloc(24);
	b = kmalloc(80);

	cr_assert_not_null(a, "kmalloc returned NULL");
	cr_assert_not_null(b, "kmalloc returned NULL");
	cr_assert(is_kheap_aligned(a), "kmalloc returned misaligned pointer");
	cr_assert(is_kheap_aligned(b), "kmalloc returned misaligned pointer");
	cr_assert_neq(a, b, "distinct allocations returned the same pointer");
	cr_assert_lt(kheap_free_bytes(), kheap_total_bytes(), "free space did not decrease");

	kfree(a);
	reused = kmalloc(24);

	cr_assert_eq(reused, a, "allocator did not reuse the freed block");
}

Test(kheap, strdup_allocates_kernel_copy) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	char*                         copy;

	init_test_kheap(arena, sizeof(arena));

	copy = strdup("owned-name");
	cr_assert_not_null(copy, "strdup returned NULL");
	cr_assert_str_eq(copy, "owned-name");
	cr_assert_neq(copy, "owned-name", "strdup should return a distinct allocation");

	copy[0] = 'O';
	cr_assert_str_eq(copy, "Owned-name");

	kfree(copy);
}

Test(kheap, strndup_limits_source_scan_and_terminates_copy) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	const char                    source[] = {'a', 'b', 'c', 'd'};
	char*                         copy;

	init_test_kheap(arena, sizeof(arena));

	copy = strndup(source, 3u);
	cr_assert_not_null(copy, "strndup returned NULL");
	cr_assert_str_eq(copy, "abc");

	kfree(copy);
}
