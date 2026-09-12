#include <test_memory.h>

#include "../address_space/test_support.h"

static _Alignas(TEST_MAPPING_GRANULE) uint8_t arena[KiB(192)];

Test(address_transfer, implicit_zero_reads_and_writes_do_not_require_ptes) {
	struct address_space space = {0};
	struct mapping*      mapping;
	void*                base;
	uint8_t              bytes[32], pattern[32];
	init_test_address_space(arena, sizeof(arena));
	cr_assert(address_space_create_process(&space));
	cr_assert(test_address_space_map(&space,
	                                 2u * TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 0u,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &mapping,
	                                 &base));
	memset(bytes, 0xff, sizeof(bytes));
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base + TEST_MAPPING_GRANULE - 8u, bytes, sizeof(bytes)),
	             ADDRESS_TRANSFER_OK);
	for (size_t i = 0u; i < sizeof(bytes); i++) cr_assert_eq(bytes[i], 0u);
	cr_assert_eq(mock_paging_mapping_count(), 0u, "logical read created user PTEs");
	for (size_t i = 0u; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i + 1u);
	cr_assert_eq(address_space_copy_to(&space, (uintptr_t)base + TEST_MAPPING_GRANULE - 8u, pattern, sizeof(pattern)),
	             ADDRESS_TRANSFER_OK);
	memset(bytes, 0, sizeof(bytes));
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base + TEST_MAPPING_GRANULE - 8u, bytes, sizeof(bytes)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(bytes, pattern, sizeof(bytes));
	cr_assert_eq(mock_paging_mapping_count(), 0u, "logical write created user PTEs");
	cr_assert(address_space_unmap(&space, mapping));
	mapping_release(mapping);
	address_space_destroy_process(&space);
}

Test(address_transfer, crosses_mappings_and_enforces_protection) {
	struct address_space space = {0};
	struct mapping *     first_mapping, *second_mapping;
	void*                first;
	uint8_t              in[16], out[16];
	init_test_address_space(arena, sizeof(arena));
	cr_assert(address_space_create_process(&space));
	uintptr_t base = space.base + 4u * TEST_MAPPING_GRANULE;
	cr_assert(test_address_space_map(&space,
	                                 TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 base,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &first_mapping,
	                                 &first));
	cr_assert(test_address_space_map(&space,
	                                 TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 base + TEST_MAPPING_GRANULE,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &second_mapping,
	                                 NULL));
	for (size_t i = 0u; i < sizeof(in); i++) in[i] = (uint8_t)(0xa0u + i);
	cr_assert_eq(address_space_copy_to(&space, base + TEST_MAPPING_GRANULE - 8u, in, sizeof(in)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(&space, base + TEST_MAPPING_GRANULE - 8u, out, sizeof(out)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(in, out, sizeof(in));
	cr_assert(address_space_protect(&space, second_mapping, MEMORY_ACCESS_READ));
	cr_assert_eq(address_space_copy_to(&space, base + TEST_MAPPING_GRANULE, in, 1u), ADDRESS_TRANSFER_ACCESS_DENIED);
	cr_assert(address_space_unmap(&space, second_mapping));
	cr_assert(address_space_unmap(&space, first_mapping));
	mapping_release(second_mapping);
	mapping_release(first_mapping);
	address_space_destroy_process(&space);
}

Test(address_transfer, overlapping_copy_has_memmove_semantics) {
	struct address_space space = {0};
	struct mapping*      mapping;
	void*                base;
	uint8_t              initial[512], expected[512], actual[512];
	init_test_address_space(arena, sizeof(arena));
	cr_assert(address_space_create_process(&space));
	cr_assert(test_address_space_map(&space,
	                                 TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 0u,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &mapping,
	                                 &base));
	for (size_t i = 0u; i < sizeof(initial); i++) initial[i] = (uint8_t)i;
	memcpy(expected, initial, sizeof(expected));
	memmove(expected + 37u, expected, 400u);
	cr_assert_eq(address_space_copy_to(&space, (uintptr_t)base, initial, sizeof(initial)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_between(&space, (uintptr_t)base + 37u, &space, (uintptr_t)base, 400u),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base, actual, sizeof(actual)), ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(actual, expected, sizeof(actual));
	cr_assert(address_space_unmap(&space, mapping));
	mapping_release(mapping);
	address_space_destroy_process(&space);
}
