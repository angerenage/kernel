#include <core/pmm.h>

#include "vmm_test.h"

Test(vmm, rolls_back_partial_mappings_on_map_failure) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	void*                  virt = NULL;
	size_t                 free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	mock_paging_fail_after(1);

	cr_assert(!vmm_alloc_pages(3, 1, VMM_PAGE_WRITE, &virt), "vmm_alloc_pages unexpectedly succeeded");
	cr_assert_null(virt, "vmm_alloc_pages should leave out_virt as NULL on failure");
	cr_assert_eq(mock_paging_mapping_count(), 0, "rollback left mappings behind");
	cr_assert_eq(pmm_free_page_count(), free_before, "rollback leaked physical pages");
}
