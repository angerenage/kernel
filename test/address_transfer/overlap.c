#include "test_support.h"

Test(address_transfer, overlapping_same_space_copy_preserves_memmove_semantics_across_pages) {
	_Alignas(4096) uint8_t  arena[KiB(512)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 2u,
			.align_pages = 1u,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
    };
	vmm_id_t  id   = VMM_ID_INVALID;
	void*     base = NULL;
	uint8_t   initial[32];
	uint8_t   expected[32];
	uint8_t   actual[32];
	uintptr_t window;

	for (size_t i = 0u; i < sizeof(initial); i++) initial[i] = (uint8_t)(0x20u + i);
	memcpy(expected, initial, sizeof(expected));
	memmove(expected + 4u, expected, 24u);

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &id, &base), "failed to allocate user transfer range");

	window = (uintptr_t)base + PMM_PAGE_SIZE - 16u;
	cr_assert_eq(address_space_copy_to(&user_space, window, initial, sizeof(initial)),
	             ADDRESS_TRANSFER_OK,
	             "failed to seed overlap test data");

	cr_assert_eq(address_space_copy_between(&user_space, window + 4u, &user_space, window, 24u),
	             ADDRESS_TRANSFER_OK,
	             "overlapping same-space transfer failed");

	memset(actual, 0, sizeof(actual));
	cr_assert_eq(address_space_copy_from(&user_space, window, actual, sizeof(actual)),
	             ADDRESS_TRANSFER_OK,
	             "failed to read back overlap test data");
	cr_assert_eq(memcmp(actual, expected, sizeof(actual)),
	             0,
	             "cross-page overlap must behave like one memmove over the complete range");

	cr_assert(vmm_free(&user_space, id), "failed to free overlap test allocation");
	vmm_address_space_deinit(&user_space);
}
