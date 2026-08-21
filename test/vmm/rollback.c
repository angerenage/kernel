#include "test_support.h"

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

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "vmm_alloc unexpectedly succeeded");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed eager allocation left tracking metadata behind");
	cr_assert_eq(mock_paging_mapping_count(), 0, "rollback left mappings behind");
	cr_assert_eq(pmm_free_page_count(), free_before, "rollback leaked physical pages");
}

Test(vmm, map_all_rollback_preserves_preexisting_mappings_and_backing) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 3u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
		.map_flags  = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        id         = VMM_ID_INVALID;
	void*           base       = NULL;
	uintptr_t       first_phys = 0u;
	uintptr_t       after_phys = 0u;
	size_t          free_before;
	size_t          backed_pages;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "lazy allocation failed");
	cr_assert(vmm_map(address_space_kernel(), id), "initial full mapping failed");
	backed_pages = vmm_test_pages_consumed_since(free_before);
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &first_phys, NULL),
	          "initial first-page query failed");
	cr_assert(vmm_unmap(address_space_kernel(), id, false), "backing-preserving unmap failed");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "could not establish the pre-existing partial mapping");

	mock_paging_fail_once_after(mock_paging_mapping_count() + 1u);
	cr_assert_not(vmm_map(address_space_kernel(), id), "map_all unexpectedly survived the injected failure");
	cr_assert_eq(mock_paging_mapping_count(), 1u, "rollback did not restore the original mapping set");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &after_phys, NULL),
	          "rollback removed the mapping that existed before map_all");
	cr_assert_eq(after_phys, first_phys, "rollback changed the pre-existing mapping's backing");
	cr_assert_not(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base + PMM_PAGE_SIZE, NULL, NULL),
	              "rollback retained a mapping created by the failed operation");
	cr_assert(vmm_query_id(address_space_kernel(), id, &info), "query failed after map_all rollback");
	cr_assert_eq(info.state, VMM_STATE_PARTIAL, "rollback did not restore the original partial state");
	cr_assert_eq(vmm_test_pages_consumed_since(free_before),
	             backed_pages,
	             "rollback changed the set of pre-existing physical backing pages");

	cr_assert(vmm_map(address_space_kernel(), id), "map_all could not be retried after rollback");
	cr_assert(vmm_free(address_space_kernel(), id), "rollback test cleanup failed");
	cr_assert_eq(pmm_free_page_count(), free_before, "rollback test leaked physical pages or mapping metadata");
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

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed");

	mock_paging_fail_once_after(mock_paging_mapping_count() + 1u);
	cr_assert(!vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL),
	          "vmm_protect unexpectedly succeeded");

	for (size_t page = 0; page < params.page_count; page++) {
		uintptr_t virt = (uintptr_t)base + page * (uintptr_t)PMM_PAGE_SIZE;

		cr_assert(hal_paging_query(hal_paging_kernel_space(), virt, NULL, &flags),
		          "hal_paging_query failed after protect rollback");
		cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "protect rollback did not restore the original flags");
	}

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed after protect rollback");
	cr_assert_eq(info.prot, params.prot, "failed protect changed the tracked protection");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "failed protect changed the tracked state");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after protect rollback");
	cr_assert_eq(mock_paging_mapping_count(), 0, "free leaked mappings after protect rollback");
	cr_assert_eq(pmm_free_page_count(), free_before, "free leaked physical pages after protect rollback");
}

Test(vmm, failed_lazy_fault_never_leaves_a_freed_page_as_live_backing) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        id         = VMM_ID_INVALID;
	void*           base       = NULL;
	uintptr_t       held[64]   = {0};
	size_t          held_count = 0u;
	bool            aliased    = false;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "lazy allocation failed");

	mock_paging_fail_once_after(mock_paging_mapping_count());
	cr_assert_not(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	              "fault resolution unexpectedly succeeded while the paging backend rejected the map");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "failed fault resolution must not leave a live page-table mapping");
	cr_assert(vmm_query_id(address_space_kernel(), id, &info),
	          "failed fault resolution must keep the tracked allocation valid");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "failed first fault must leave the allocation unmapped");

	/* Retained backing must still be owned; released backing must be cleared. */
	if (info.first_phys != 0u) {
		while (held_count < sizeof(held) / sizeof(held[0])) {
			uintptr_t phys = 0u;

			if (!pmm_alloc_pages(1u, &phys)) break;
			held[held_count++] = phys;
			if (phys == info.first_phys) aliased = true;
		}
	}

	for (size_t i = 0u; i < held_count; i++) {
		cr_assert(pmm_free_pages(held[i], 1u), "failed to restore PMM probe allocation");
	}

	cr_assert_not(aliased, "backing store retained a physical address that had already been returned to the PMM");
	cr_assert(vmm_free(address_space_kernel(), id),
	          "allocation must remain normally destroyable after a failed lazy fault");
}

Test(vmm, failed_lazy_fault_can_be_retried_cleanly) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 1u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
		.map_flags  = VMM_MAP_LAZY,
	};
	vmm_id_t id   = VMM_ID_INVALID;
	void*    base = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "lazy allocation failed");
	mock_paging_fail_once_after(0u);
	cr_assert_not(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	              "first fault must surface the injected map failure");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "a one-shot mapping failure must not poison the backing store for a later retry");
	cr_assert_eq(mock_paging_mapping_count(), 1u, "retry must create exactly one mapping");
	cr_assert(vmm_free(address_space_kernel(), id), "retry allocation cleanup failed");
}
