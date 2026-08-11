#include "test_support.h"

Test(address_transfer, copies_between_kernel_and_user_and_between_user_spaces) {
	_Alignas(4096) uint8_t arena[KiB(1024)];
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)arena,
         .length = sizeof(arena),
         .type   = MEM_RANGE_USABLE,
		 },
	};
	struct address_space    left_space  = {0};
	struct address_space    right_space = {0};
	struct vmm_alloc_params params      = {
			 .page_count  = 1,
			 .align_pages = 1,
			 .prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			 .kind        = VMM_KIND_GENERIC,
			 .map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t                     left_id    = VMM_ID_INVALID;
	vmm_id_t                     right_id   = VMM_ID_INVALID;
	void*                        left_base  = NULL;
	void*                        right_base = NULL;
	const char                   source[]   = "transfer-across-user-pages";
	char                         out[sizeof(source)];
	uintptr_t                    left_addr;
	uintptr_t                    right_addr;
	enum address_transfer_result transfer_result;

	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(address_space_init(&left_space, MM_USER_VMM_BASE, 16u), "failed to initialize left user address space");
	cr_assert(hal_paging_space_create(&left_space.hal_space), "failed to create left user HAL address space");
	cr_assert(address_space_init(&right_space, MM_USER_VMM_BASE, 16u), "failed to initialize right user address space");
	cr_assert(hal_paging_space_create(&right_space.hal_space), "failed to create right user HAL address space");
	cr_assert(vmm_alloc(&left_space, &params, &left_id, &left_base), "failed to allocate left user range");

	left_addr = (uintptr_t)left_base;
	transfer_result =
		address_space_validate_range(&left_space,
	                                 left_addr,
	                                 sizeof(source),
	                                 ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	cr_assert_eq(transfer_result, ADDRESS_TRANSFER_OK, "range fault-in failed with result %d", transfer_result);

	transfer_result = address_space_copy_to(&left_space, left_addr, source, sizeof(source));
	cr_assert_eq(transfer_result, ADDRESS_TRANSFER_OK, "kernel-to-user copy failed with result %d", transfer_result);
	memset(out, 0, sizeof(out));
	cr_assert_eq(address_space_copy_from(&left_space, left_addr, out, sizeof(out)),
	             ADDRESS_TRANSFER_OK,
	             "user-to-kernel copy failed");
	cr_assert_eq(memcmp(out, source, sizeof(source)), 0, "user-to-kernel copy returned wrong data");

	cr_assert(vmm_alloc(&right_space, &params, &right_id, &right_base), "failed to allocate right user range");
	right_addr = (uintptr_t)right_base;
	cr_assert_eq(address_space_copy_between(&right_space, right_addr, &left_space, left_addr, sizeof(source)),
	             ADDRESS_TRANSFER_OK,
	             "user-to-user copy failed");
	memset(out, 0, sizeof(out));
	cr_assert_eq(address_space_copy_from(&right_space, right_addr, out, sizeof(out)),
	             ADDRESS_TRANSFER_OK,
	             "right user-to-kernel copy failed");
	cr_assert_eq(memcmp(out, source, sizeof(source)), 0, "user-to-user copy returned wrong data");

	cr_assert(vmm_free(&right_space, right_id), "vmm_free failed for right user range");
	cr_assert(vmm_free(&left_space, left_id), "vmm_free failed for left user range");
	vmm_address_space_deinit(&right_space);
	vmm_address_space_deinit(&left_space);
}

Test(address_transfer, typed_address_transfer_helpers_allow_unaligned_cross_page_access) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 2,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t  alloc_id      = VMM_ID_INVALID;
	void*     base          = NULL;
	uint64_t  u64_value     = 0x1122334455667788ull;
	uint64_t  read_u64      = 0u;
	uintptr_t uptr_value    = (uintptr_t)0xfeedc0de12345678ull;
	uintptr_t read_uintptr  = 0u;
	uint32_t  rejected_read = 0u;
	uintptr_t cross_page_addr;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "failed to allocate typed helper user range");

	cross_page_addr = (uintptr_t)base + PMM_PAGE_SIZE - 3u;
	cr_assert_eq(address_space_write_u64(&user_space, cross_page_addr, u64_value),
	             ADDRESS_TRANSFER_OK,
	             "unaligned cross-page u64 write failed");
	cr_assert_eq(address_space_read_u64(&user_space, cross_page_addr, &read_u64),
	             ADDRESS_TRANSFER_OK,
	             "unaligned cross-page u64 read failed");
	cr_assert_eq(read_u64, u64_value, "u64 helper round-trip returned the wrong value");

	cr_assert_eq(address_space_write_uintptr(&user_space, (uintptr_t)base, uptr_value),
	             ADDRESS_TRANSFER_OK,
	             "uintptr write failed");
	cr_assert_eq(address_space_read_uintptr(&user_space, (uintptr_t)base, &read_uintptr),
	             ADDRESS_TRANSFER_OK,
	             "uintptr read failed");
	cr_assert_eq(read_uintptr, uptr_value, "uintptr helper round-trip returned the wrong value");

	cr_assert_eq(address_space_read_u32(&user_space, (uintptr_t)base + 2u * PMM_PAGE_SIZE, &rejected_read),
	             ADDRESS_TRANSFER_NOT_MAPPED,
	             "read outside allocation should fail");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for typed helper user range");
	vmm_address_space_deinit(&user_space);
}
