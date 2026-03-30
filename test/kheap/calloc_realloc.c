#include "kheap_test.h"

Test(kheap, calloc_zeroes_and_realloc_preserves_contents) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	uint8_t*                      data;
	uint8_t*                      grown;

	init_test_kheap(arena, sizeof(arena));

	data = (uint8_t*)kcalloc(32, sizeof(uint8_t));
	cr_assert_not_null(data, "kcalloc returned NULL");
	for (size_t i = 0; i < 32; i++) {
		cr_assert_eq(data[i], 0, "kcalloc did not zero memory");
		data[i] = (uint8_t)(0x40u + i);
	}

	grown = (uint8_t*)krealloc(data, 256);
	cr_assert_not_null(grown, "krealloc returned NULL");
	for (size_t i = 0; i < 32; i++) {
		cr_assert_eq(grown[i], (uint8_t)(0x40u + i), "krealloc did not preserve contents");
	}
}
