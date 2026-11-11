#include "test_support.h"

Test(pmm, rejects_overlapping_usable_ranges) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(128)];
	const struct pmm_memory_range memory_map[] = {
		{.address = (uintptr_t)arena, .size = KiB(64), .usable = true},
		{.address = (uintptr_t)(arena + KiB(32)), .size = KiB(64), .usable = true},
	};

	cr_assert_not(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u));
}

Test(pmm, rejects_overlapping_usable_and_reserved_ranges) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(128)];
	const struct pmm_memory_range memory_map[] = {
		{.address = (uintptr_t)arena, .size = KiB(64), .usable = true},
		{.address = (uintptr_t)(arena + KiB(32)), .size = KiB(8), .usable = false},
	};

	cr_assert_not(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u));
}

Test(pmm, adjacent_unordered_usable_extents_form_one_contiguous_range) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(64)];
	const struct pmm_memory_range memory_map[] = {
		{.address = (uintptr_t)(arena + KiB(32)), .size = KiB(32), .usable = true},
		{.address = (uintptr_t)arena, .size = KiB(32), .usable = true},
	};
	struct pmm_extent run;

	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u));
	cr_assert(pmm_alloc(
			&(const struct pmm_alloc_request){.size = 9u * PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE}, &run));
	cr_assert_lt(run.address, (uintptr_t)(arena + KiB(32)), "run must begin in the lower extent");
	cr_assert_gt(run.address + run.size, (uintptr_t)(arena + KiB(32)), "run must cross the original map boundary");
}

Test(pmm, trims_partial_granules_at_usable_range_boundaries) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(32)];
	const struct pmm_memory_range memory_map = {
		.address = (uintptr_t)arena + 1u,
		.size = 3u * PMM_TEST_GRANULE + PMM_TEST_GRANULE - 2u,
		.usable = true,
	};
	struct pmm_extent allocation;

	cr_assert(pmm_init(&memory_map, 1u, 0u));
	cr_assert_eq(pmm_total_size(), 2u * PMM_TEST_GRANULE);
	cr_assert(pmm_alloc(
			&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE}, &allocation));
	cr_assert_eq(allocation.address, (uintptr_t)arena + 2u * PMM_TEST_GRANULE);
}

Test(pmm, physical_address_zero_is_allocatable) {
	enum { SMALL_RANGE_COUNT = 128u };
	static _Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(2560)];
	struct pmm_memory_range memory_map[SMALL_RANGE_COUNT + 1u];
	struct pmm_extent allocation = {0};

	/* The small disjoint ranges make the metadata larger than the range at
	 * physical zero, forcing PMM metadata into the final large range. */
	for (size_t i = 0u; i < SMALL_RANGE_COUNT; i++) {
		memory_map[i] = (struct pmm_memory_range){
			.address = 2u * i * PMM_TEST_GRANULE,
			.size = PMM_TEST_GRANULE,
			.usable = true,
		};
	}
	memory_map[SMALL_RANGE_COUNT] = (struct pmm_memory_range){
		.address = KiB(2048),
		.size = KiB(512),
		.usable = true,
	};

	cr_assert(pmm_init(memory_map, SMALL_RANGE_COUNT + 1u, (uintptr_t)arena));
	cr_assert(
			pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .maximum_address = PMM_TEST_GRANULE},
					&allocation));
	cr_assert_eq(allocation.address, 0u);
	cr_assert_eq(allocation.size, PMM_TEST_GRANULE);
	cr_assert(pmm_free(allocation));
}
