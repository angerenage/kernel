#include "test_support.h"

static void init_adjacent_regions(struct address_space* space, uintptr_t base, vmm_prot_t first_prot,
                                  vmm_prot_t second_prot, vmm_id_t* out_first, vmm_id_t* out_second) {
	struct vmm_alloc_params first = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = first_prot | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
	};
	struct vmm_alloc_params second = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = second_prot | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
	};

	cr_assert(vmm_alloc_at(space, (void*)base, &first, out_first), "failed to allocate first transfer region");
	cr_assert(vmm_alloc_at(space, (void*)(base + PMM_PAGE_SIZE), &second, out_second),
	          "failed to allocate adjacent transfer region");
}

Test(address_transfer, failed_copy_to_does_not_modify_a_successful_prefix) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space     = {0};
	const uintptr_t        base           = MM_USER_VMM_BASE + 4u * (uintptr_t)PMM_PAGE_SIZE;
	const uint8_t          original[4]    = {0xa1u, 0xa2u, 0xa3u, 0xa4u};
	const uint8_t          replacement[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
	uint8_t                actual[4]      = {0};
	vmm_id_t               writable_id    = VMM_ID_INVALID;
	vmm_id_t               readonly_id    = VMM_ID_INVALID;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	init_adjacent_regions(&user_space, base, VMM_PROT_READ | VMM_PROT_WRITE, VMM_PROT_READ, &writable_id, &readonly_id);

	cr_assert_eq(
		address_space_copy_to(&user_space, base + PMM_PAGE_SIZE - sizeof(original), original, sizeof(original)),
		ADDRESS_TRANSFER_OK,
		"failed to initialize writable prefix");

	cr_assert_eq(address_space_copy_to(&user_space, base + PMM_PAGE_SIZE - 4u, replacement, sizeof(replacement)),
	             ADDRESS_TRANSFER_ACCESS_DENIED,
	             "copy crossing into read-only memory must fail");

	cr_assert_eq(address_space_copy_from(&user_space, base + PMM_PAGE_SIZE - sizeof(actual), actual, sizeof(actual)),
	             ADDRESS_TRANSFER_OK,
	             "failed to inspect writable prefix after rejected copy");
	cr_assert_eq(memcmp(actual, original, sizeof(actual)),
	             0,
	             "failed copy_to must not silently modify a prefix when no partial count is reported");

	cr_assert(vmm_free(&user_space, readonly_id), "failed to free read-only region");
	cr_assert(vmm_free(&user_space, writable_id), "failed to free writable region");
	vmm_address_space_deinit(&user_space);
}

Test(address_transfer, failed_copy_from_does_not_partially_overwrite_kernel_buffer) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space    = {0};
	const uintptr_t        base          = MM_USER_VMM_BASE + 4u * (uintptr_t)PMM_PAGE_SIZE;
	const uint8_t          user_bytes[4] = {0xb1u, 0xb2u, 0xb3u, 0xb4u};
	uint8_t                output[8];
	uint8_t                expected[8];
	vmm_id_t               readable_id   = VMM_ID_INVALID;
	vmm_id_t               unreadable_id = VMM_ID_INVALID;

	memset(output, 0x5au, sizeof(output));
	memcpy(expected, output, sizeof(expected));

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	init_adjacent_regions(
		&user_space, base, VMM_PROT_READ | VMM_PROT_WRITE, VMM_PROT_WRITE, &readable_id, &unreadable_id);

	cr_assert_eq(
		address_space_copy_to(&user_space, base + PMM_PAGE_SIZE - sizeof(user_bytes), user_bytes, sizeof(user_bytes)),
		ADDRESS_TRANSFER_OK,
		"failed to initialize readable prefix");

	cr_assert_eq(address_space_copy_from(&user_space, base + PMM_PAGE_SIZE - 4u, output, sizeof(output)),
	             ADDRESS_TRANSFER_ACCESS_DENIED,
	             "copy crossing into non-readable memory must fail");
	cr_assert_eq(memcmp(output, expected, sizeof(output)),
	             0,
	             "failed copy_from must not partially overwrite the destination buffer");

	cr_assert(vmm_free(&user_space, unreadable_id), "failed to free unreadable region");
	cr_assert(vmm_free(&user_space, readable_id), "failed to free readable region");
	vmm_address_space_deinit(&user_space);
}
