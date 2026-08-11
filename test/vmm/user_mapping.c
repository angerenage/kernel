#include "test_support.h"

Test(vmm, maps_and_reprotects_user_pages_distinct_from_kernel_pages) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 1,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
    };
	struct vmm_info           info;
	vmm_id_t                  alloc_id = VMM_ID_INVALID;
	void*                     base     = NULL;
	uint64_t                  flags    = 0;
	struct hal_address_space* user_hal_space;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base),
	          "kernel vmm_alloc accepted a user mapping");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed kernel user allocation changed the allocation id");
	cr_assert_null(base, "failed kernel user allocation changed the allocation base");

	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	user_hal_space = address_space_hal(&user_space);
	cr_assert_not_null(user_hal_space, "user address space did not create a HAL paging handle");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "vmm_alloc failed for a user mapping");
	cr_assert(vmm_query_id(&user_space, alloc_id, &info), "vmm_query_id failed for user mapping");
	cr_assert_eq(info.prot, params.prot, "vmm_query_id lost the user protection bit");
	cr_assert(!hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, NULL),
	          "user mapping leaked into the kernel address space");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed for user address-space mapping");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "user mapping flags were not translated");

	cr_assert(!vmm_protect(&user_space, alloc_id, VMM_PROT_READ | VMM_PROT_WRITE),
	          "vmm_protect allowed clearing user access inside a user address space");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after rejected protect");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "rejected protect changed user mapping flags");

	cr_assert(vmm_protect(&user_space, alloc_id, VMM_PROT_READ | VMM_PROT_EXEC | VMM_PROT_USER),
	          "vmm_protect failed to reprotect user access");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after reprotecting user access");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_EXEC | HAL_PAGE_USER), "user exec mapping flags were not translated");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for user mapping");
	vmm_address_space_deinit(&user_space);
}
