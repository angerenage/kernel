#include <base/vmm.h>

#include "../vmm/test_support.h"

static _Alignas(VMM_PAGE_SIZE) uint8_t arena[KiB(192)];

Test(address_transfer, implicit_zero_reads_and_writes_do_not_require_ptes) {
	struct address_space space = {0};
	vmm_id_t             id;
	void*                base;
	uint8_t              bytes[32], pattern[32];
	init_test_vmm(arena, sizeof(arena));
	cr_assert(vm_space_create_user(&space));
	cr_assert(test_vm_map(&space, 2u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &id, &base));
	memset(bytes, 0xff, sizeof(bytes));
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base + VMM_PAGE_SIZE - 8u, bytes, sizeof(bytes)),
	             ADDRESS_TRANSFER_OK);
	for (size_t i = 0u; i < sizeof(bytes); i++) cr_assert_eq(bytes[i], 0u);
	cr_assert_eq(mock_paging_mapping_count(), 0u, "logical read created user PTEs");
	for (size_t i = 0u; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i + 1u);
	cr_assert_eq(address_space_copy_to(&space, (uintptr_t)base + VMM_PAGE_SIZE - 8u, pattern, sizeof(pattern)),
	             ADDRESS_TRANSFER_OK);
	memset(bytes, 0, sizeof(bytes));
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base + VMM_PAGE_SIZE - 8u, bytes, sizeof(bytes)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(bytes, pattern, sizeof(bytes));
	cr_assert_eq(mock_paging_mapping_count(), 0u, "logical write created user PTEs");
	cr_assert(vm_space_unmap(&space, id));
	vm_space_destroy(&space);
}

Test(address_transfer, crosses_mappings_and_enforces_protection) {
	struct address_space space = {0};
	vmm_id_t             first_id, second_id;
	void*                first;
	uint8_t              in[16], out[16];
	init_test_vmm(arena, sizeof(arena));
	cr_assert(vm_space_create_user(&space));
	uintptr_t base = space.base + 4u * VMM_PAGE_SIZE;
	cr_assert(test_vm_map(&space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, base, 1u, 0u, &first_id, &first));
	cr_assert(test_vm_map(&space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, base + VMM_PAGE_SIZE, 1u, 0u, &second_id, NULL));
	for (size_t i = 0u; i < sizeof(in); i++) in[i] = (uint8_t)(0xa0u + i);
	cr_assert_eq(address_space_copy_to(&space, base + VMM_PAGE_SIZE - 8u, in, sizeof(in)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(&space, base + VMM_PAGE_SIZE - 8u, out, sizeof(out)), ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(in, out, sizeof(in));
	cr_assert(vm_space_protect(&space, second_id, VMM_PROT_READ));
	cr_assert_eq(address_space_copy_to(&space, base + VMM_PAGE_SIZE, in, 1u), ADDRESS_TRANSFER_ACCESS_DENIED);
	cr_assert(vm_space_unmap(&space, second_id));
	cr_assert(vm_space_unmap(&space, first_id));
	vm_space_destroy(&space);
}

Test(address_transfer, overlapping_copy_has_memmove_semantics) {
	struct address_space space = {0};
	vmm_id_t             id;
	void*                base;
	uint8_t              initial[512], expected[512], actual[512];
	init_test_vmm(arena, sizeof(arena));
	cr_assert(vm_space_create_user(&space));
	cr_assert(test_vm_map(&space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &id, &base));
	for (size_t i = 0u; i < sizeof(initial); i++) initial[i] = (uint8_t)i;
	memcpy(expected, initial, sizeof(expected));
	memmove(expected + 37u, expected, 400u);
	cr_assert_eq(address_space_copy_to(&space, (uintptr_t)base, initial, sizeof(initial)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_between(&space, (uintptr_t)base + 37u, &space, (uintptr_t)base, 400u),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base, actual, sizeof(actual)), ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(actual, expected, sizeof(actual));
	cr_assert(vm_space_unmap(&space, id));
	vm_space_destroy(&space);
}
