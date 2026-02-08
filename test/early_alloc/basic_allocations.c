#include "early_alloc_test.h"

Test(early_alloc, basic_allocations) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	init_early_allocator(arena, sizeof(arena));

	struct {
		size_t size;
		size_t align;
	} cases[] = {
		{   1,    1},
		{  16,   16},
		{  24,    8},
		{  64,   64},
		{4096, 4096},
	};

	uint64_t before = early_remaining_bytes();

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t sz = cases[i].size;
		size_t al = cases[i].align;

		uint64_t r0 = early_remaining_bytes();
		void*    p  = early_alloc(sz, al);
		uint64_t r1 = early_remaining_bytes();

		cr_assert_not_null(p, "early_alloc returned NULL");
		cr_assert(is_aligned_uintptr((uintptr_t)p, al), "early_alloc returned misaligned pointer");
		cr_assert(r1 <= r0, "early_remaining_bytes increased after early_alloc");

		uint8_t* b = (uint8_t*)p;
		for (size_t j = 0; j < sz; j++) {
			b[j] = (uint8_t)(0xA0u + (uint8_t)j);
		}
	}

	uint64_t after = early_remaining_bytes();
	cr_assert(after <= before, "early_remaining_bytes increased overall");
}
