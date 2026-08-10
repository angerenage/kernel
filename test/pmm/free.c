#include "test_support.h"

#define PMM_TEST_USABLE_PAGES ((PMM_TEST_LOW_LENGTH + PMM_TEST_HIGH_LENGTH) / PMM_PAGE_SIZE)

static bool page_was_allocated(const uintptr_t* pages, size_t page_count, uintptr_t phys) {
	for (size_t i = 0; i < page_count; i++) {
		if (pages[i] == phys) return true;
	}
	return false;
}

Test(pmm, allocator_metadata_pages_are_permanently_reserved) {
	_Alignas(PMM_PAGE_SIZE) uint8_t arena[KiB(256)];
	uintptr_t                       allocated[PMM_TEST_USABLE_PAGES] = {0};
	size_t                          allocated_count                  = 0u;
	size_t                          initial_free;
	uintptr_t                       reserved_page = 0u;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_page_count();

	while (allocated_count < PMM_TEST_USABLE_PAGES) {
		uintptr_t phys = 0u;

		if (!pmm_alloc_pages(1u, &phys)) break;
		allocated[allocated_count++] = phys;
	}

	cr_assert_eq(allocated_count, initial_free, "exhaustion must return every free page exactly once");
	cr_assert_eq(pmm_free_page_count(), 0u, "allocator must be exhausted before probing reserved pages");

	for (size_t page = 0u; page < PMM_TEST_LOW_LENGTH / PMM_PAGE_SIZE && reserved_page == 0u; page++) {
		uintptr_t phys = (uintptr_t)(arena + PMM_TEST_LOW_OFFSET) + page * PMM_PAGE_SIZE;
		if (!page_was_allocated(allocated, allocated_count, phys)) reserved_page = phys;
	}
	for (size_t page = 0u; page < PMM_TEST_HIGH_LENGTH / PMM_PAGE_SIZE && reserved_page == 0u; page++) {
		uintptr_t phys = (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET) + page * PMM_PAGE_SIZE;
		if (!page_was_allocated(allocated, allocated_count, phys)) reserved_page = phys;
	}

	cr_assert_neq(reserved_page, 0u, "test map must contain at least one PMM-reserved metadata page");
	cr_assert_not(pmm_free_pages(reserved_page, 1u),
	              "pmm_free_pages must reject pages permanently reserved for allocator metadata");
	cr_assert_eq(pmm_free_page_count(), 0u, "failed reserved-page free must not alter the free counter");
}

Test(pmm, failed_free_is_atomic_and_preserves_accounting) {
	_Alignas(PMM_PAGE_SIZE) uint8_t arena[KiB(256)];
	uintptr_t                       run = 0u;
	size_t                          initial_free;
	size_t                          after_partial_free;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_page_count();

	cr_assert_not(pmm_free_pages((uintptr_t)arena + 1u, 1u), "misaligned physical address must be rejected");
	cr_assert_eq(pmm_free_page_count(), initial_free, "misaligned free must not alter accounting");

	cr_assert_not(pmm_free_pages((uintptr_t)(arena + KiB(48)), 1u),
	              "address outside all managed ranges must be rejected");
	cr_assert_eq(pmm_free_page_count(), initial_free, "out-of-range free must not alter accounting");

	cr_assert(pmm_alloc_pages(2u, &run), "two-page allocation failed");
	cr_assert_eq(pmm_free_page_count(), initial_free - 2u, "allocation accounting mismatch");

	cr_assert(pmm_free_pages(run + PMM_PAGE_SIZE, 1u), "freeing the second page failed");
	after_partial_free = pmm_free_page_count();
	cr_assert_eq(after_partial_free, initial_free - 1u, "partial free accounting mismatch");

	cr_assert_not(pmm_free_pages(run, 2u),
	              "free containing an already-free page must fail instead of partially clearing the run");
	cr_assert_eq(pmm_free_page_count(), after_partial_free, "failed mixed-state free must be atomic");

	cr_assert(pmm_free_pages(run, 1u), "the still-allocated first page must remain releasable");
	cr_assert_eq(pmm_free_page_count(), initial_free, "round trip must restore the original free count");
}
