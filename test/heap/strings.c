#include <libc/string.h>

#include "test_support.h"

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
