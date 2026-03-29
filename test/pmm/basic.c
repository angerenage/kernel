#include "pmm_test.h"

Test(pmm, allocates_every_free_page_and_reuses_freed_page) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              first_page = 0;
	size_t                 initial_free;
	size_t                 total_pages;
	size_t                 allocated = 0;

	init_test_pmm(arena, sizeof(arena));

	initial_free = pmm_free_page_count();
	total_pages  = pmm_total_page_count();

	cr_assert_gt(pmm_managed_range_count(), 0, "pmm did not manage any ranges");
	cr_assert_gt(total_pages, initial_free, "pmm did not reserve metadata pages");
	cr_assert_gt(initial_free, 0, "pmm reported no free pages");

	cr_assert(pmm_alloc_pages(1, &first_page), "pmm_alloc_pages failed");
	cr_assert_eq(first_page & (PMM_PAGE_SIZE - 1u), 0, "allocated page is not page-aligned");
	cr_assert(phys_in_range(first_page, (uintptr_t)(arena + PMM_TEST_LOW_OFFSET), PMM_TEST_LOW_LENGTH) ||
	              phys_in_range(first_page, (uintptr_t)(arena + PMM_TEST_HIGH_OFFSET), PMM_TEST_HIGH_LENGTH),
	          "allocated page fell outside usable ranges");

	allocated = 1;
	while (1) {
		uintptr_t phys = 0;

		if (!pmm_alloc_pages(1, &phys)) break;
		allocated++;
	}

	cr_assert_eq(allocated, initial_free, "allocator did not hand out exactly the free page count");
	cr_assert_eq(pmm_free_page_count(), 0, "allocator should be exhausted");

	cr_assert(pmm_free_pages(first_page, 1), "pmm_free_pages failed");
	cr_assert_eq(pmm_free_page_count(), 1, "free page count did not recover");

	{
		uintptr_t reused = 0;

		cr_assert(pmm_alloc_pages(1, &reused), "pmm_alloc_pages failed after free");
		cr_assert_eq(reused, first_page, "allocator did not reuse the freed page first");
	}
}
