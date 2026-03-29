#include "pmm_test.h"

Test(pmm, allocates_and_frees_contiguous_runs) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	size_t                 before_free;
	uintptr_t              run = 0;

	init_test_pmm(arena, sizeof(arena));

	before_free = pmm_free_page_count();

	cr_assert(pmm_alloc_pages(3, &run), "pmm_alloc_pages failed for a contiguous run");
	cr_assert_eq(run & (PMM_PAGE_SIZE - 1u), 0, "contiguous run is not page-aligned");
	cr_assert_eq(pmm_free_page_count(), before_free - 3, "free page count did not shrink by three");

	cr_assert(pmm_free_pages(run, 3), "pmm_free_pages failed for a contiguous run");
	cr_assert_eq(pmm_free_page_count(), before_free, "free page count did not recover after freeing a run");
	cr_assert(!pmm_free_pages(run, 3), "double-free should be rejected");

	{
		uintptr_t impossible = 0;

		cr_assert(!pmm_alloc_pages(before_free + 1, &impossible), "allocator unexpectedly found too many pages");
	}
}
