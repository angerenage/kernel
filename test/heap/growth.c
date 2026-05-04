#include "heap_test.h"

Test(heap, grows_when_initial_arena_is_exhausted) {
	_Alignas(4096) static uint8_t arena[KiB(128)];
	void*                         blocks[128] = {0};
	size_t                        count       = 0;
	size_t                        before_total;

	init_test_heap(arena, sizeof(arena));

	before_total = heap_total_bytes();

	while (count < (sizeof(blocks) / sizeof(blocks[0]))) {
		blocks[count] = malloc(256);
		if (blocks[count] == NULL) break;
		count++;
	}

	cr_assert_gt(count, 16, "heap growth test did not allocate enough blocks");
	cr_assert_gt(heap_total_bytes(), before_total, "heap did not request additional pages");
}
