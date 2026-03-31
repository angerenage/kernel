#include <core/pmm.h>
#include <hal/paging.h>

#include "vmm_test.h"

Test(vmm, allocates_maps_and_frees_pages) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	void*                  virt  = NULL;
	uintptr_t              phys  = 0;
	uint64_t               flags = 0;
	size_t                 free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc_pages(2, 4, VMM_PAGE_WRITE | VMM_PAGE_GLOBAL, &virt), "vmm_alloc_pages failed");
	cr_assert_not_null(virt, "vmm_alloc_pages returned NULL");
	cr_assert_eq(
		((uintptr_t)virt) & ((uintptr_t)(4 * PMM_PAGE_SIZE) - 1u), 0, "virtual allocation did not honor alignment");
	cr_assert_eq(mock_paging_mapping_count(), 2, "mock paging mapping count mismatch");
	cr_assert_eq(pmm_free_page_count(), free_before - 2, "pmm free page count mismatch after allocation");

	cr_assert(hal_paging_query((uintptr_t)virt, &phys, &flags), "hal_paging_query failed");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "mapped physical page is not aligned");
	cr_assert_eq(flags, (uint64_t)(VMM_PAGE_WRITE | VMM_PAGE_GLOBAL), "mapped flags mismatch");

	cr_assert(vmm_free_pages(virt, 2), "vmm_free_pages failed");
	cr_assert_eq(mock_paging_mapping_count(), 0, "mock paging mappings leaked after free");
	cr_assert_eq(pmm_free_page_count(), free_before, "pmm free page count did not recover after free");
}
