#include "test_support.h"

Test(vmm, allocates_queries_and_frees_mapped_ranges) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 4,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       phys     = 0;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "vmm_alloc failed");
	cr_assert_neq(alloc_id, VMM_ID_INVALID, "vmm_alloc returned an invalid id");
	cr_assert_not_null(base, "vmm_alloc returned NULL base");
	cr_assert_eq(
		((uintptr_t)base) & ((uintptr_t)(4 * PMM_PAGE_SIZE) - 1u), 0, "vmm_alloc did not honor virtual alignment");
	cr_assert_eq(vmm_count(address_space_kernel()), 1, "vmm_count mismatch after alloc");
	cr_assert_eq(mock_paging_mapping_count(), 2, "mapped allocation did not create page mappings");
	cr_assert_geq(vmm_test_pages_consumed_since(free_before), 2, "mapped allocation did not consume backing pages");

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed");
	cr_assert_eq(info.base, base, "vmm_query_id returned the wrong base");
	cr_assert_eq(info.page_count, 2, "vmm_query_id returned the wrong size");
	cr_assert_eq(info.kind, VMM_KIND_HEAP, "vmm_query_id returned the wrong kind");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "vmm_query_id returned the wrong state");
	cr_assert_eq(info.prot, params.prot, "vmm_query_id returned the wrong protection");

	cr_assert(vmm_query(address_space_kernel(), (uint8_t*)base + PMM_PAGE_SIZE, &info),
	          "vmm_query failed for interior address");
	cr_assert_eq(info.id, alloc_id, "vmm_query returned the wrong allocation");

	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &phys, &flags),
	          "hal_paging_query failed for mapped allocation");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "mapped allocation physical address is not aligned");
	cr_assert_eq(flags, (uint64_t)(VMM_PROT_WRITE | VMM_PROT_GLOBAL), "mapped allocation flags mismatch");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "vmm_count mismatch after free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "vmm_free leaked physical pages");
}

Test(vmm, allocates_at_exact_addresses_and_rejects_overlaps) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        fixed_id  = VMM_ID_INVALID;
	vmm_id_t        mapped_id = VMM_ID_INVALID;
	uintptr_t       fixed     = (uintptr_t)MM_KERNEL_VMM_BASE + 8u * (uintptr_t)PMM_PAGE_SIZE;
	uintptr_t       mapped    = fixed + 4u * (uintptr_t)PMM_PAGE_SIZE;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc_at(address_space_kernel(), (void*)fixed, &params, &fixed_id),
	          "vmm_alloc_at failed for a fixed lazy range");
	cr_assert(vmm_query_id(address_space_kernel(), fixed_id, &info), "fixed allocation was not tracked");
	cr_assert_eq((uintptr_t)info.base, fixed, "fixed allocation base mismatch");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "fixed lazy allocation should start reserved");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fixed lazy allocation unexpectedly mapped pages");

	cr_assert(!vmm_alloc_at(address_space_kernel(), (void*)(fixed + PMM_PAGE_SIZE), &params, &mapped_id),
	          "vmm_alloc_at unexpectedly allowed an overlapping range");
	cr_assert_eq(mapped_id, VMM_ID_INVALID, "failed fixed allocation changed the id");

	params.map_flags = 0u;
	cr_assert(vmm_alloc_at(address_space_kernel(), (void*)mapped, &params, &mapped_id),
	          "vmm_alloc_at failed for a fixed eager range");
	cr_assert(vmm_query_id(address_space_kernel(), mapped_id, &info), "fixed eager allocation was not tracked");
	cr_assert_eq((uintptr_t)info.base, mapped, "fixed eager allocation base mismatch");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "fixed eager allocation should start mapped");
	cr_assert_eq(mock_paging_mapping_count(), 2, "fixed eager allocation did not map both pages");

	cr_assert(vmm_free(address_space_kernel(), mapped_id), "failed to free fixed eager allocation");
	cr_assert(vmm_free(address_space_kernel(), fixed_id), "failed to free fixed lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fixed allocations leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "fixed allocations leaked physical pages");
}

Test(vmm, rejects_invalid_protection_masks) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params bad_params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | ((vmm_prot_t)1ull << 63),
		.kind        = VMM_KIND_GENERIC,
	};
	struct vmm_alloc_params good_params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	uint64_t flags    = 0;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &bad_params, &alloc_id, &base),
	          "vmm_alloc accepted invalid protection bits");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed vmm_alloc left tracked allocations behind");

	cr_assert(vmm_alloc(address_space_kernel(), &good_params, &alloc_id, &base),
	          "vmm_alloc failed for valid protection bits");
	cr_assert(!vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | ((vmm_prot_t)1ull << 63)),
	          "vmm_protect accepted invalid protection bits");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after rejected protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "rejected protect changed the live mapping");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after invalid protection tests");
}

Test(vmm, rejects_guard_pages_for_non_stack_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.guard_pages = 1,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base),
	          "vmm_alloc unexpectedly accepted guard pages for a heap allocation");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed guard-page validation changed the allocation id");
	cr_assert_null(base, "failed guard-page validation changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed guard-page validation left tracked allocations behind");
}
