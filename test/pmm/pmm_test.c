#include "pmm_test.h"

bool phys_in_range(uintptr_t phys, uintptr_t base, size_t length) {
	return phys >= base && phys < base + length;
}

void init_test_pmm(uint8_t* arena, size_t arena_size) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uint64_t)(uintptr_t)(arena + PMM_TEST_LOW_OFFSET),
         .length = PMM_TEST_LOW_LENGTH,
         .type   = MEM_RANGE_USABLE,
		 },
		{
         .base   = (uint64_t)(uintptr_t)(arena + KiB(32)),
         .length = KiB(8),
         .type   = MEM_RANGE_RESERVED,
		 },
		{
         .base   = (uint64_t)(uintptr_t)(arena + PMM_TEST_HIGH_OFFSET),
         .length = PMM_TEST_HIGH_LENGTH,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	cr_assert_geq(arena_size, KiB(128), "test arena is too small");
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
}
