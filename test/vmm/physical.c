#include "test_support.h"

Test(vmm, unmapping_external_physical_memory_never_transfers_ownership_to_the_vmm) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              external_phys = 0u;
	vmm_id_t               id            = VMM_ID_INVALID;
	void*                  base          = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(pmm_alloc_pages(1u, &external_phys), "test could not acquire caller-owned physical memory");
	cr_assert(
		vmm_alloc_phys(address_space_kernel(), external_phys, 1u, VMM_PROT_READ | VMM_PROT_WRITE, NULL, &id, &base),
		"external physical mapping failed");

	cr_assert(vmm_unmap(address_space_kernel(), id, true), "external physical mapping could not be unmapped");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "external physical unmap left a live mapping");
	cr_assert(vmm_free(address_space_kernel(), id), "external physical region metadata could not be destroyed");

	cr_assert(pmm_free_pages(external_phys, 1u), "VMM released caller-owned physical memory during unmap");
}

Test(vmm, external_physical_mapping_obeys_address_space_protection_policy) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space    = {0};
	uintptr_t              external_phys = 0u;
	vmm_id_t               id            = VMM_ID_INVALID;
	void*                  base          = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(pmm_alloc_pages(1u, &external_phys), "test could not acquire external physical page");

	cr_assert_not(
		vmm_alloc_phys(address_space_kernel(), external_phys, 1u, VMM_PROT_READ | VMM_PROT_USER, NULL, &id, &base),
		"kernel address space must reject USER physical mappings");
	cr_assert_eq(id, VMM_ID_INVALID, "rejected kernel physical mapping changed the id");
	cr_assert_null(base, "rejected kernel physical mapping changed the base");

	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize test user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create test user paging space");

	cr_assert_not(vmm_alloc_phys(&user_space, external_phys, 1u, VMM_PROT_READ, NULL, &id, &base),
	              "user address space must reject physical mappings without USER access");
	cr_assert_not(
		vmm_alloc_phys(
			&user_space, external_phys, 1u, VMM_PROT_READ | VMM_PROT_USER | VMM_PROT_GLOBAL, NULL, &id, &base),
		"user address space must reject GLOBAL physical mappings");

	vmm_address_space_deinit(&user_space);
	cr_assert(pmm_free_pages(external_phys, 1u),
	          "policy rejection must preserve caller ownership of the physical page");
}

Test(vmm, external_physical_ranges_reject_address_overflow_atomically) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	const uintptr_t        last_page = UINTPTR_MAX & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	vmm_id_t               id        = (vmm_id_t)0x1234u;
	void*                  base      = (void*)(uintptr_t)0x1234u;
	size_t                 free_before;
	size_t                 regions_before;
	size_t                 mappings_before;

	init_test_vmm(arena, sizeof(arena));
	free_before     = pmm_free_page_count();
	regions_before  = vmm_count(address_space_kernel());
	mappings_before = mock_paging_mapping_count();

	cr_assert_not(vmm_alloc_phys(address_space_kernel(), last_page, 2u, VMM_PROT_READ, NULL, &id, &base),
	              "physical range wrapping past UINTPTR_MAX must be rejected");
	cr_assert_eq(id, VMM_ID_INVALID, "overflow failure must clear the output id");
	cr_assert_null(base, "overflow failure must clear the output base");
	cr_assert_eq(vmm_count(address_space_kernel()), regions_before, "overflow failure left region metadata behind");
	cr_assert_eq(mock_paging_mapping_count(), mappings_before, "overflow failure created page-table mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "overflow failure consumed PMM pages");
}
