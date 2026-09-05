#include "test_support.h"

Test(pmm, allocates_and_frees_contiguous_extents) {
	_Alignas(PMM_TEST_ARENA_ALIGNMENT) uint8_t arena[KiB(256)];
	size_t                                     before_free;
	struct pmm_extent                          run = {0};

	init_test_pmm(arena, sizeof(arena));

	before_free = pmm_free_size();

	cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = 3u * PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
	                    &run));
	cr_assert_eq(run.address & (PMM_TEST_GRANULE - 1u), 0, "contiguous run is not granule-aligned");
	cr_assert_eq(pmm_free_size(), before_free - run.size, "free size did not shrink by the extent size");

	cr_assert(pmm_free(run), "pmm_free failed for a contiguous run");
	cr_assert_eq(pmm_free_size(), before_free, "free size did not recover after freeing a run");
	cr_assert(!pmm_free(run), "double-free should be rejected");

	{
		struct pmm_extent impossible;

		cr_assert(!pmm_alloc(
			&(const struct pmm_alloc_request){.size = before_free + PMM_TEST_GRANULE, .alignment = PMM_TEST_GRANULE},
			&impossible));
	}
}
