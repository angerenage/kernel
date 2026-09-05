#include "test_support.h"

#define PMM_TEST_USABLE_GRANULES ((PMM_TEST_LOW_LENGTH + PMM_TEST_HIGH_LENGTH) / PMM_TEST_GRANULE)

static bool extent_address_was_allocated(const uintptr_t* addresses, size_t count, uintptr_t address) {
	for (size_t i = 0; i < count; i++) {
		if (addresses[i] == address) return true;
	}
	return false;
}

Test(pmm, allocator_metadata_granules_are_permanently_reserved) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	uintptr_t                                  allocated[PMM_TEST_USABLE_GRANULES];
	size_t                                     allocated_count = 0u;
	size_t                                     initial_free;
	uintptr_t                                  reserved_address = 0u;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_size();

	while (allocated_count < PMM_TEST_USABLE_GRANULES) {
		struct pmm_extent extent;

		if (!pmm_alloc(&(const struct pmm_alloc_request){.size = PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
		               &extent))
			break;
		allocated[allocated_count++] = extent.address;
	}

	cr_assert_eq(
		allocated_count * PMM_TEST_GRANULE, initial_free, "exhaustion must return all free memory exactly once");
	cr_assert_eq(pmm_free_size(), 0u, "allocator must be exhausted before probing reserved granules");

	for (size_t granule = 0u; granule < PMM_TEST_LOW_LENGTH / PMM_TEST_GRANULE && reserved_address == 0u; granule++) {
		uintptr_t address = (uintptr_t)(arena + PMM_TEST_LOW_OFFSET) + granule * PMM_TEST_GRANULE;
		if (!extent_address_was_allocated(allocated, allocated_count, address)) reserved_address = address;
	}
	for (size_t granule = 0u; granule < PMM_TEST_HIGH_LENGTH / PMM_TEST_GRANULE && reserved_address == 0u; granule++) {
		uintptr_t address = (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET) + granule * PMM_TEST_GRANULE;
		if (!extent_address_was_allocated(allocated, allocated_count, address)) reserved_address = address;
	}

	cr_assert_neq(reserved_address, 0u, "test map must contain at least one PMM-reserved metadata granule");
	cr_assert_not(pmm_free((struct pmm_extent){.address = reserved_address, .size = PMM_TEST_GRANULE}),
	              "pmm_free must reject memory permanently reserved for allocator metadata");
	cr_assert_eq(pmm_free_size(), 0u, "failed reserved-granule free must not alter the free counter");
}

Test(pmm, failed_free_is_atomic_and_preserves_accounting) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	struct pmm_extent                          run;
	size_t                                     initial_free;
	size_t                                     after_partial_free;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_size();

	cr_assert_not(pmm_free((struct pmm_extent){.address = (uintptr_t)arena + 1u, .size = PMM_TEST_GRANULE}),
	              "misaligned physical address must be rejected");
	cr_assert_eq(pmm_free_size(), initial_free, "misaligned free must not alter accounting");

	cr_assert_not(pmm_free((struct pmm_extent){.address = (uintptr_t)(arena + KiB(48)), .size = PMM_TEST_GRANULE}),
	              "address outside all managed ranges must be rejected");
	cr_assert_eq(pmm_free_size(), initial_free, "out-of-range free must not alter accounting");

	cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = 2u * PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
	                    &run));
	cr_assert_eq(pmm_free_size(), initial_free - run.size, "allocation accounting mismatch");

	cr_assert(pmm_free((struct pmm_extent){.address = run.address + PMM_TEST_GRANULE, .size = PMM_TEST_GRANULE}),
	          "freeing the second granule failed");
	after_partial_free = pmm_free_size();
	cr_assert_eq(after_partial_free, initial_free - PMM_TEST_GRANULE, "partial free accounting mismatch");

	cr_assert_not(pmm_free(run),
	              "free containing an already-free granule must fail instead of partially clearing the extent");
	cr_assert_eq(pmm_free_size(), after_partial_free, "failed mixed-state free must be atomic");

	cr_assert(pmm_free((struct pmm_extent){.address = run.address, .size = PMM_TEST_GRANULE}),
	          "the still-allocated first granule must remain releasable");
	cr_assert_eq(pmm_free_size(), initial_free, "round trip must restore the original free count");
}
