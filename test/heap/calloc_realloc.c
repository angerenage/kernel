#include <limits.h>
#include <string.h>

#include "test_support.h"

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

Test(heap, failed_realloc_preserves_original_allocation_and_contents) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	uint8_t*                      ptr;
	void*                         failed;
	size_t                        before_total;
	size_t                        before_free;

	init_test_heap(arena, sizeof(arena));
	ptr = malloc(128u);
	cr_assert_not_null(ptr);
	for (size_t i = 0u; i < 128u; i++) ptr[i] = (uint8_t)(i ^ 0x5au);

	before_total = heap_total_bytes();
	before_free  = heap_free_bytes();
	failed       = realloc(ptr, SIZE_MAX - 64u);

	cr_assert_null(failed, "oversized realloc must fail");
	for (size_t i = 0u; i < 128u; i++) {
		cr_assert_eq(ptr[i], (uint8_t)(i ^ 0x5au), "failed realloc must preserve the original allocation");
	}
	cr_assert_eq(heap_total_bytes(), before_total, "failed realloc must not leave useless growth arenas behind");
	cr_assert_eq(heap_free_bytes(), before_free, "failed realloc must not consume additional heap capacity");

	free(ptr);
	cr_assert_eq(heap_free_bytes(),
	             heap_total_bytes(),
	             "original allocation must remain normally releasable after realloc failure");
}

Test(heap, calloc_overflow_is_side_effect_free) {
	_Alignas(4096) static uint8_t arena[KiB(64)];
	size_t                        before_total;
	size_t                        before_free;

	init_test_heap(arena, sizeof(arena));
	before_total = heap_total_bytes();
	before_free  = heap_free_bytes();

	cr_assert_null(calloc(SIZE_MAX, 2u), "calloc multiplication overflow must fail");
	cr_assert_eq(heap_total_bytes(), before_total, "calloc overflow must not grow the heap");
	cr_assert_eq(heap_free_bytes(), before_free, "calloc overflow must not consume heap capacity");
}
