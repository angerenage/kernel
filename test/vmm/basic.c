#include <core/pmm.h>
#include <core/vmm.h>
#include <hal/paging.h>

#include "test_support.h"

static size_t pages_consumed_since(size_t free_before) {
	size_t free_after = pmm_free_page_count();
	return free_before >= free_after ? (free_before - free_after) : 0;
}

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

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "vmm_alloc failed");
	cr_assert_neq(alloc_id, VMM_ID_INVALID, "vmm_alloc returned an invalid id");
	cr_assert_not_null(base, "vmm_alloc returned NULL base");
	cr_assert_eq(
		((uintptr_t)base) & ((uintptr_t)(4 * PMM_PAGE_SIZE) - 1u), 0, "vmm_alloc did not honor virtual alignment");
	cr_assert_eq(vmm_count(), 1, "vmm_count mismatch after alloc");
	cr_assert_eq(mock_paging_mapping_count(), 2, "mapped allocation did not create page mappings");
	cr_assert_geq(pages_consumed_since(free_before), 2, "mapped allocation did not consume backing pages");

	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed");
	cr_assert_eq(info.base, base, "vmm_query_id returned the wrong base");
	cr_assert_eq(info.page_count, 2, "vmm_query_id returned the wrong size");
	cr_assert_eq(info.kind, VMM_KIND_HEAP, "vmm_query_id returned the wrong kind");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "vmm_query_id returned the wrong state");
	cr_assert_eq(info.prot, params.prot, "vmm_query_id returned the wrong protection");

	cr_assert(vmm_query((uint8_t*)base + PMM_PAGE_SIZE, &info), "vmm_query failed for interior address");
	cr_assert_eq(info.id, alloc_id, "vmm_query returned the wrong allocation");

	cr_assert(hal_paging_query((uintptr_t)base, &phys, &flags), "hal_paging_query failed for mapped allocation");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "mapped allocation physical address is not aligned");
	cr_assert_eq(flags, (uint64_t)(VMM_PROT_WRITE | VMM_PROT_GLOBAL), "mapped allocation flags mismatch");

	cr_assert(vmm_free(alloc_id), "vmm_free failed");
	cr_assert_eq(vmm_count(), 0, "vmm_count mismatch after free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "vmm_free leaked physical pages");
}

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
	uint64_t        flags;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert_eq(vmm_count(), 1, "vmm_count mismatch for lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation unexpectedly created mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation unexpectedly consumed physical pages");

	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed for lazy allocation");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "lazy allocation did not start reserved");

	cr_assert(vmm_map(alloc_id), "vmm_map failed");
	cr_assert_eq(mock_paging_mapping_count(), 3, "vmm_map did not map every page");
	cr_assert_geq(pages_consumed_since(free_before), 3, "vmm_map did not consume physical pages");
	cr_assert(hal_paging_query((uintptr_t)base, &first_phys, &flags), "hal_paging_query failed after vmm_map");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "vmm_map set incorrect initial flags");

	cr_assert(vmm_unmap(alloc_id, false), "vmm_unmap(false) failed");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_unmap(false) leaked mappings");
	cr_assert_geq(pages_consumed_since(free_before), 3, "vmm_unmap(false) unexpectedly freed backing pages");

	cr_assert(vmm_map(alloc_id), "vmm_map failed when reusing preserved backing");
	cr_assert(hal_paging_query((uintptr_t)base, &remapped_phys, &flags), "hal_paging_query failed after remap");
	cr_assert_eq(remapped_phys, first_phys, "vmm_map did not reuse preserved backing");

	cr_assert(vmm_protect(alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL), "vmm_protect failed");
	cr_assert(hal_paging_query((uintptr_t)base, NULL, &flags), "hal_paging_query failed after vmm_protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_GLOBAL, "vmm_protect did not update page flags");

	cr_assert(vmm_free(alloc_id), "vmm_free failed for lazy allocation");
	cr_assert_eq(vmm_count(), 0, "vmm_count mismatch after lazy allocation free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation free leaked physical pages");
}

Test(vmm, resolves_page_faults_for_lazy_heap_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       phys     = 0;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert(vmm_resolve_page_fault((uintptr_t)base + PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy reserved allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "fault resolution mapped more than the faulting page");
	cr_assert_lt(pages_consumed_since(free_before),
	             params.page_count + 1u,
	             "fault resolution allocated backing for the entire heap allocation");
	cr_assert(!hal_paging_query((uintptr_t)base, NULL, NULL),
	          "fault resolution unexpectedly mapped a non-faulting heap page");
	cr_assert(hal_paging_query((uintptr_t)base + PMM_PAGE_SIZE, NULL, &flags),
	          "fault resolution did not map the faulting address");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "fault resolution applied incorrect mapping flags");
	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed after fault resolution");
	cr_assert_eq(info.state, VMM_STATE_PARTIAL, "fault resolution did not leave the heap allocation partially mapped");
	cr_assert_eq(info.first_phys, 0, "fault resolution unexpectedly reported backing for the untouched first page");

	cr_assert(vmm_resolve_page_fault((uintptr_t)base), "vmm_resolve_page_fault failed for the first heap page");
	cr_assert_eq(mock_paging_mapping_count(), 2, "second fault resolution did not map the remaining heap page");
	cr_assert_geq(pages_consumed_since(free_before),
	              params.page_count,
	              "second fault resolution did not allocate the remaining heap backing");
	cr_assert(hal_paging_query((uintptr_t)base, &phys, NULL),
	          "second fault resolution did not map the first heap page");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "heap fault resolution returned an unaligned physical page");
	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed after resolving every heap page");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "heap allocation did not become fully mapped");

	cr_assert(vmm_free(alloc_id), "vmm_free failed after fault resolution");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fault resolution cleanup leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "fault resolution cleanup leaked physical pages");
}

Test(vmm, resolves_page_faults_for_lazy_stack_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_STACK,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "lazy stack vmm_alloc failed");
	cr_assert(vmm_resolve_page_fault((uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy stack allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "stack fault resolution mapped more than the faulting page");
	cr_assert_lt(pages_consumed_since(free_before),
	             params.page_count + 1u,
	             "stack fault resolution allocated backing for the entire stack allocation");
	cr_assert(!hal_paging_query((uintptr_t)base, NULL, NULL),
	          "stack fault resolution unexpectedly mapped the base page");
	cr_assert(!hal_paging_query((uintptr_t)base + PMM_PAGE_SIZE, NULL, NULL),
	          "stack fault resolution unexpectedly mapped the middle page");
	cr_assert(hal_paging_query((uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE, NULL, &flags),
	          "stack fault resolution did not map the faulting address");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "stack fault resolution applied incorrect mapping flags");
	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed after stack fault resolution");
	cr_assert_eq(info.kind, VMM_KIND_STACK, "fault resolution changed the stack allocation kind");
	cr_assert_eq(
		info.state, VMM_STATE_PARTIAL, "stack fault resolution did not leave the stack allocation partially mapped");

	cr_assert(vmm_free(alloc_id), "vmm_free failed after stack fault resolution");
	cr_assert_eq(mock_paging_mapping_count(), 0, "stack fault resolution cleanup leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "stack fault resolution cleanup leaked physical pages");
}

Test(vmm, refuses_page_fault_resolution_for_non_lazy_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "eager vmm_alloc failed");
	cr_assert(vmm_unmap(alloc_id, false), "vmm_unmap(false) failed for eager allocation");
	cr_assert(!vmm_resolve_page_fault((uintptr_t)base),
	          "vmm_resolve_page_fault unexpectedly remapped a non-lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "non-lazy fault resolution changed live mappings");
	cr_assert(!vmm_resolve_page_fault((uintptr_t)base + KiB(128)),
	          "vmm_resolve_page_fault unexpectedly succeeded for an unknown address");

	cr_assert(vmm_free(alloc_id), "vmm_free failed after rejected fault resolution");
	cr_assert_eq(pmm_free_page_count(), free_before, "rejected fault resolution leaked physical pages");
}

Test(vmm, rolls_back_partial_mappings_on_eager_map_failure) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	mock_paging_fail_after(1);

	cr_assert(!vmm_alloc(&params, &alloc_id, &base), "vmm_alloc unexpectedly succeeded");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(), 0, "failed eager allocation left tracking metadata behind");
	cr_assert_eq(mock_paging_mapping_count(), 0, "rollback left mappings behind");
	cr_assert_eq(pmm_free_page_count(), free_before, "rollback leaked physical pages");
}

Test(vmm, rolls_back_failed_protect_and_preserves_original_mapping) {
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
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(&params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert(vmm_map(alloc_id), "vmm_map failed");

	mock_paging_fail_once_after(mock_paging_mapping_count() + 1u);
	cr_assert(!vmm_protect(alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL), "vmm_protect unexpectedly succeeded");

	for (size_t page = 0; page < params.page_count; page++) {
		uintptr_t virt = (uintptr_t)base + page * (uintptr_t)PMM_PAGE_SIZE;

		cr_assert(hal_paging_query(virt, NULL, &flags), "hal_paging_query failed after protect rollback");
		cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "protect rollback did not restore the original flags");
	}

	cr_assert(vmm_query_id(alloc_id, &info), "vmm_query_id failed after protect rollback");
	cr_assert_eq(info.prot, params.prot, "failed protect changed the tracked protection");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "failed protect changed the tracked state");

	cr_assert(vmm_free(alloc_id), "vmm_free failed after protect rollback");
	cr_assert_eq(mock_paging_mapping_count(), 0, "free leaked mappings after protect rollback");
	cr_assert_eq(pmm_free_page_count(), free_before, "free leaked physical pages after protect rollback");
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

	cr_assert(!vmm_alloc(&bad_params, &alloc_id, &base), "vmm_alloc accepted invalid protection bits");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(), 0, "failed vmm_alloc left tracked allocations behind");

	cr_assert(vmm_alloc(&good_params, &alloc_id, &base), "vmm_alloc failed for valid protection bits");
	cr_assert(!vmm_protect(alloc_id, VMM_PROT_READ | ((vmm_prot_t)1ull << 63)),
	          "vmm_protect accepted invalid protection bits");
	cr_assert(hal_paging_query((uintptr_t)base, NULL, &flags), "hal_paging_query failed after rejected protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "rejected protect changed the live mapping");

	cr_assert(vmm_free(alloc_id), "vmm_free failed after invalid protection tests");
}
