#include "test_support.h"

Test(address_transfer, rejects_invalid_access_masks_without_faulting_pages) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 1u,
			.align_pages = 1u,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t id   = VMM_ID_INVALID;
	void*    base = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &id, &base), "failed to allocate lazy validation range");
	free_before = pmm_free_page_count();

	cr_assert_eq(address_space_validate_range(&user_space, (uintptr_t)base, 1u, ADDRESS_TRANSFER_READ | (1u << 31)),
	             ADDRESS_TRANSFER_INVALID_ARGUMENTS,
	             "unknown access flags must be rejected");
	cr_assert_eq(
		address_space_validate_range(&user_space,
	                                 (uintptr_t)base,
	                                 1u,
	                                 ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_PRESENT | ADDRESS_TRANSFER_FAULT_IN),
		ADDRESS_TRANSFER_INVALID_ARGUMENTS,
		"PRESENT and FAULT_IN are mutually exclusive");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "invalid validation requests must not materialize lazy pages");
	cr_assert_eq(pmm_free_page_count(), free_before, "invalid validation requests must not allocate physical backing");

	cr_assert(vmm_free(&user_space, id), "failed to free validation allocation");
	vmm_address_space_deinit(&user_space);
}

Test(address_transfer, detects_range_overflow_before_touching_address_space_state) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct address_space   user_space = {0};
	uint8_t                scratch[8] = {0};
	size_t                 free_before;
	size_t                 mappings_before;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	free_before     = pmm_free_page_count();
	mappings_before = mock_paging_mapping_count();

	cr_assert_eq(
		address_space_validate_range(&user_space, UINTPTR_MAX - 3u, 8u, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER),
		ADDRESS_TRANSFER_ADDRESS_OVERFLOW,
		"validation must reject a wrapping address range");
	cr_assert_eq(address_space_copy_from(&user_space, UINTPTR_MAX - 3u, scratch, sizeof(scratch)),
	             ADDRESS_TRANSFER_ADDRESS_OVERFLOW,
	             "copy_from must reject a wrapping source range");
	cr_assert_eq(address_space_copy_to(&user_space, UINTPTR_MAX - 3u, scratch, sizeof(scratch)),
	             ADDRESS_TRANSFER_ADDRESS_OVERFLOW,
	             "copy_to must reject a wrapping destination range");
	cr_assert_eq(mock_paging_mapping_count(), mappings_before, "overflow rejection must not alter mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "overflow rejection must not allocate backing pages");

	vmm_address_space_deinit(&user_space);
}

Test(address_transfer, validates_user_ranges_without_faulting_unless_requested) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 2,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "failed to allocate lazy user range");

	cr_assert_eq(
		address_space_validate_range(&user_space, (uintptr_t)base, 1u, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER),
		ADDRESS_TRANSFER_OK,
		"reserved user address should be valid by VMM metadata");
	cr_assert_eq(address_space_validate_range(&user_space,
	                                          (uintptr_t)base,
	                                          2u * PMM_PAGE_SIZE,
	                                          ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_PRESENT),
	             ADDRESS_TRANSFER_NOT_MAPPED,
	             "present validation should not fault in lazy pages");
	cr_assert_eq(
		address_space_validate_range(&user_space,
	                                 (uintptr_t)base,
	                                 2u * PMM_PAGE_SIZE,
	                                 ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN),
		ADDRESS_TRANSFER_OK,
		"fault-in validation should materialize lazy pages");
	cr_assert_eq(
		address_space_validate_range(
			&user_space, (uintptr_t)base + 2u * PMM_PAGE_SIZE, 1u, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER),
		ADDRESS_TRANSFER_NOT_MAPPED,
		"address after allocation should be rejected");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for user range");
	vmm_address_space_deinit(&user_space);
}
