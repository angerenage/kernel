#include "test_support.h"

bool phys_in_range(uintptr_t phys, uintptr_t base, size_t length) {
	return phys >= base && phys < base + length;
}

void init_test_pmm(uint8_t* arena, size_t arena_size) {
	const struct pmm_memory_range memory_map[] = {
		{
			.address = (uint64_t)(uintptr_t)(arena + PMM_TEST_LOW_OFFSET),
			.size = PMM_TEST_LOW_LENGTH,
			.usable = true,
		},
		{
			.address = (uint64_t)(uintptr_t)(arena + KiB(32)),
			.size = KiB(8),
			.usable = false,
		},
		{
			.address = (uint64_t)(uintptr_t)(arena + PMM_TEST_HIGH_OFFSET),
			.size = PMM_TEST_HIGH_LENGTH,
			.usable = true,
		},
	};

	cr_assert_geq(arena_size, KiB(128), "test arena is too small");
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
}
