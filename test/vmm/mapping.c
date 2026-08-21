#include "test_support.h"

Test(vmm, supports_lazy_map_unmap_remap_and_reprotect) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       first_phys;
	uintptr_t       remapped_phys;
	uint64_t*       preserved_data;
	uint64_t        flags;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert_eq(vmm_count(address_space_kernel()), 1, "vmm_count mismatch for lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation unexpectedly created mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation unexpectedly consumed physical pages");

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed for lazy allocation");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "lazy allocation did not start reserved");

	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed");
	cr_assert_eq(mock_paging_mapping_count(), 3, "vmm_map did not map every page");
	cr_assert_geq(vmm_test_pages_consumed_since(free_before), 3, "vmm_map did not consume physical pages");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &first_phys, &flags),
	          "hal_paging_query failed after vmm_map");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "vmm_map set incorrect initial flags");
	preserved_data  = (uint64_t*)(first_phys + boot_info.direct_map_offset);
	*preserved_data = UINT64_C(0x4b45524e454c564d);

	cr_assert(vmm_unmap(address_space_kernel(), alloc_id, false), "vmm_unmap(address_space_kernel(), false) failed");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_unmap(address_space_kernel(), false) leaked mappings");
	cr_assert_geq(vmm_test_pages_consumed_since(free_before),
	              3,
	              "vmm_unmap(address_space_kernel(), false) unexpectedly freed backing pages");
	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "query failed after preserving backing");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "unmapped region with backing must be reserved");
	cr_assert_eq(info.first_phys, first_phys, "unmapping discarded preserved physical backing");

	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed when reusing preserved backing");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &remapped_phys, &flags),
	          "hal_paging_query failed after remap");
	cr_assert_eq(remapped_phys, first_phys, "vmm_map did not reuse preserved backing");
	cr_assert_eq(*preserved_data, UINT64_C(0x4b45524e454c564d), "remapping did not preserve backing contents");

	cr_assert(vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL), "vmm_protect failed");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after vmm_protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_GLOBAL, "vmm_protect did not update page flags");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed for lazy allocation");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "vmm_count mismatch after lazy allocation free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation free leaked physical pages");
}

Test(vmm, unmap_with_release_drops_owned_backing_but_keeps_the_region_reserved) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 2u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
	};
	struct vmm_info info;
	vmm_id_t        id   = VMM_ID_INVALID;
	void*           base = NULL;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "eager allocation failed");
	cr_assert(vmm_unmap(address_space_kernel(), id, true), "owned backing release failed");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "physical release left mappings behind");
	cr_assert_eq(pmm_free_page_count(), free_before, "physical release retained owned pages or metadata");
	cr_assert(vmm_query_id(address_space_kernel(), id, &info), "released region was no longer tracked");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "released unmapped region must be reserved");
	cr_assert_eq(info.first_phys, 0u, "released region retained a physical backing address");

	cr_assert(vmm_map(address_space_kernel(), id), "released region could not allocate fresh backing");
	cr_assert(vmm_free(address_space_kernel(), id), "released/remapped region cleanup failed");
	cr_assert_eq(pmm_free_page_count(), free_before, "released/remapped region leaked physical pages");
}
