#include "heap_test.h"

Test(heap, calloc_zeroes_and_realloc_preserves_contents) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	uint8_t*                      data;
	uint8_t*                      grown;

	init_test_heap(arena, sizeof(arena));

	data = (uint8_t*)calloc(32, sizeof(uint8_t));
	cr_assert_not_null(data, "calloc returned NULL");
	for (size_t i = 0; i < 32; i++) {
		cr_assert_eq(data[i], 0, "calloc did not zero memory");
		data[i] = (uint8_t)(0x40u + i);
	}

	grown = (uint8_t*)realloc(data, 256);
	cr_assert_not_null(grown, "realloc returned NULL");
	for (size_t i = 0; i < 32; i++) {
		cr_assert_eq(grown[i], (uint8_t)(0x40u + i), "realloc did not preserve contents");
	}
}
