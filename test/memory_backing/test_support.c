#include "test_support.h"

_Alignas(65536) uint8_t backing_test_arena[BACKING_TEST_ARENA_SIZE];

void backing_test_pmm_init(void) {
	const struct pmm_memory_range memory_map[] = {
		{
			.address = (uintptr_t)backing_test_arena,
			.size = sizeof(backing_test_arena),
			.usable = true,
		},
	};
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u));
}

size_t backing_test_granule(void) {
	return pmm_info()->allocation_granule;
}
