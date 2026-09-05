#include "test_support.h"

Test(pmm, allocates_all_free_memory_and_reuses_a_freed_extent) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          first = {0};
	size_t                                     initial_free;
	size_t                                     total_size;
	size_t                                     allocated = 0;

	init_test_pmm(arena, sizeof(arena));

	initial_free = pmm_free_size();
	total_size   = pmm_total_size();

	cr_assert_gt(pmm_managed_range_count(), 0, "pmm did not manage any ranges");
	cr_assert_gt(total_size, initial_free, "pmm did not reserve metadata memory");
	cr_assert_gt(initial_free, 0, "pmm reported no free memory");

	cr_assert(
		pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE}, &first));
	cr_assert_eq(first.address & (PMM_TEST_GRANULE - 1u), 0, "allocated extent is not granule-aligned");
	cr_assert(phys_in_range(first.address, (uintptr_t)(arena + PMM_TEST_LOW_OFFSET), PMM_TEST_LOW_LENGTH) ||
	              phys_in_range(first.address, (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET), PMM_TEST_HIGH_LENGTH),
	          "allocated extent fell outside usable ranges");

	allocated = 1;
	while (1) {
		struct pmm_extent extent;

		if (!pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
		               &extent))
			break;
		allocated++;
	}

	cr_assert_eq(allocated * PMM_TEST_GRANULE, initial_free, "allocator did not hand out exactly the free size");
	cr_assert_eq(pmm_free_size(), 0, "allocator should be exhausted");

	cr_assert(pmm_free(first), "pmm_free failed");
	cr_assert_eq(pmm_free_size(), PMM_TEST_GRANULE, "free size did not recover");

	{
		struct pmm_extent reused;

		cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
		                    &reused));
		cr_assert_eq(reused.address, first.address, "allocator did not reuse the freed extent first");
	}
}

Test(pmm, failed_allocation_does_not_change_accounting) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          extent = {.address = (uintptr_t)-1, .size = SIZE_MAX};
	size_t                                     initial_free;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_size();

	cr_assert_not(pmm_alloc(
		&(const struct pmm_alloc_request){.size = initial_free + PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
		&extent));
	cr_assert_eq(pmm_free_size(), initial_free, "failed allocation must leave free-size accounting unchanged");
}

Test(pmm, constrained_allocation_respects_window_and_alignment) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          extent;
	uintptr_t                                  min;
	uintptr_t                                  max;

	init_test_pmm(arena, sizeof(arena));
	min = (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET);
	max = min + PMM_TEST_HIGH_LENGTH;

	cr_assert(pmm_alloc(
		&(const struct pmm_alloc_request){
			.size            = 2u * PMM_TEST_GRANULE,
			.alignment       = 2u * PMM_TEST_GRANULE,
			.minimum_address = min,
			.maximum_address = max,
		},
		&extent));
	cr_assert_geq(extent.address, min);
	cr_assert_leq(extent.address + extent.size, max);
	cr_assert_eq(extent.address & (2u * PMM_TEST_GRANULE - 1u), 0u);
	cr_assert(pmm_free(extent));
}

Test(pmm, constrained_allocation_accepts_unaligned_address_bounds) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          extent;
	uintptr_t                                  base;

	init_test_pmm(arena, sizeof(arena));
	base = (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET);
	cr_assert(pmm_alloc(
		&(const struct pmm_alloc_request){
			.size            = PMM_TEST_GRANULE,
			.minimum_address = base + 1u,
			.maximum_address = base + 2u * PMM_TEST_GRANULE,
		},
		&extent));
	cr_assert_eq(extent.address, base + PMM_TEST_GRANULE);
	cr_assert_eq(extent.address + extent.size, base + 2u * PMM_TEST_GRANULE);
	cr_assert(pmm_free(extent));
}

Test(pmm, exact_claim_is_exclusive_and_reusable) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          extent;
	size_t                                     free_before;

	init_test_pmm(arena, sizeof(arena));
	cr_assert(
		pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE}, &extent));
	cr_assert(pmm_free(extent));
	free_before = pmm_free_size();

	cr_assert_eq(pmm_claim(extent), PMM_CLAIM_OK);
	cr_assert_eq(pmm_free_size(), free_before - extent.size);
	cr_assert_eq(pmm_claim(extent), PMM_CLAIM_UNAVAILABLE);
	cr_assert(pmm_free(extent));
	cr_assert_eq(pmm_claim(extent), PMM_CLAIM_OK);
	cr_assert(pmm_free(extent));
	cr_assert_eq(pmm_free_size(), free_before);
}

Test(pmm, exact_claim_distinguishes_unmanaged_physical_space) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	uintptr_t                                  outside;

	init_test_pmm(arena, sizeof(arena));
	outside = (uintptr_t)arena + sizeof(arena) + PMM_TEST_GRANULE;
	outside = (outside + PMM_TEST_GRANULE - 1u) & ~(uintptr_t)(PMM_TEST_GRANULE - 1u);
	cr_assert_eq(pmm_claim((struct pmm_extent){.address = outside, .size = PMM_TEST_GRANULE}), PMM_CLAIM_NOT_MANAGED);
}

Test(pmm, overflowing_extent_is_invalid_not_unmanaged) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          allocation;
	struct pmm_extent                          overflowing;

	init_test_pmm(arena, sizeof(arena));
	cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE}, &allocation));
	cr_assert(pmm_free(allocation));
	overflowing = (struct pmm_extent){
		.address = allocation.address,
		.size    = (size_t)(UINTPTR_MAX - allocation.address) + 1u,
	};
	cr_assert_eq(overflowing.size & (PMM_TEST_GRANULE - 1u), 0u);
	cr_assert_eq(pmm_claim(overflowing), PMM_CLAIM_UNAVAILABLE);
	cr_assert_not(pmm_free(overflowing));
}

Test(pmm, reports_a_power_of_two_allocation_granule_and_byte_accounting) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	const struct pmm_info*                     info;

	init_test_pmm(arena, sizeof(arena));
	info = pmm_info();
	cr_assert_not_null(info);
	cr_assert_neq(info->allocation_granule, 0u);
	cr_assert_eq(info->allocation_granule & (info->allocation_granule - 1u), 0u);
	cr_assert_eq(pmm_total_size() % info->allocation_granule, 0u);
	cr_assert_eq(pmm_free_size() % info->allocation_granule, 0u);
}

Test(pmm, zero_alignment_selects_the_allocation_granule) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	const struct pmm_info*                     info;
	struct pmm_extent                          extent;

	init_test_pmm(arena, sizeof(arena));
	info = pmm_info();
	cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = info->allocation_granule}, &extent));
	cr_assert_eq(extent.size, info->allocation_granule);
	cr_assert_eq(extent.address & (info->allocation_granule - 1u), 0u);
	cr_assert(pmm_free(extent));
}

Test(pmm, rejects_malformed_byte_requests_atomically) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	const struct pmm_info*                     info;
	struct pmm_extent                          output;
	size_t                                     free_before;

	init_test_pmm(arena, sizeof(arena));
	info                                     = pmm_info();
	free_before                              = pmm_free_size();
	const struct pmm_alloc_request invalid[] = {
		{.size = info->allocation_granule - 1u, .alignment = info->allocation_granule},
		{.size = info->allocation_granule, .alignment = info->allocation_granule / 2u},
		{.size = info->allocation_granule, .alignment = info->allocation_granule + 1u},
		{.size            = info->allocation_granule,
	     .alignment       = info->allocation_granule,
	     .minimum_address = (uintptr_t)arena,
	     .maximum_address = (uintptr_t)arena},
	};
	for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		output = (struct pmm_extent){.address = UINTPTR_MAX, .size = SIZE_MAX};
		cr_assert_not(pmm_alloc(&invalid[i], &output));
	}
	cr_assert_eq(pmm_free_size(), free_before);
}
