#include "test_support.h"

#include <core/address_space.h>
#include <core/mm.h>
#include <core/pmm.h>

void init_test_vmm(uint8_t* arena, size_t arena_size) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)arena,
         .length = KiB(24),
         .type   = MEM_RANGE_USABLE,
		 },
		{
         .base   = (uintptr_t)(arena + KiB(32)),
         .length = KiB(8),
         .type   = MEM_RANGE_RESERVED,
		 },
		{
         .base   = (uintptr_t)(arena + KiB(64)),
         .length = KiB(128),
         .type   = MEM_RANGE_USABLE,
		 },
	};

	cr_assert_geq(arena_size, KiB(192), "test arena is too small");
	mock_paging_reset();
	mock_cache_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(address_space_init(), "address_space_init failed");
}

size_t vmm_test_bytes_consumed_since(size_t free_before) {
	size_t free_after = pmm_free_size();
	return free_before >= free_after ? free_before - free_after : 0u;
}

bool test_vm_map(struct address_space* space, size_t page_count, memory_access_t access, uintptr_t requested_base,
                 size_t alignment_units, size_t guard_units, struct mapping** out_mapping, void** out_base) {
	struct memory*  memory;
	struct mapping* mapping;
	if ((access & ~MEMORY_ACCESS_VALID_MASK) != 0u || page_count > SIZE_MAX / TEST_MAPPING_GRANULE ||
	    !memory_create_anonymous(page_count * TEST_MAPPING_GRANULE, &memory))
		return false;
	bool mapped =
		address_space_map(space,
	                      &(const struct address_space_mapping_request){
							  .memory       = memory,
							  .address      = requested_base,
							  .alignment    = (alignment_units == 0u ? 1u : alignment_units) * TEST_MAPPING_GRANULE,
							  .guard_before = guard_units * TEST_MAPPING_GRANULE,
							  .access       = access,
						  },
	                      &mapping);
	memory_release(memory);
	if (mapped) {
		if (out_base != NULL) *out_base = (void*)mapping_address(mapping);
		if (out_mapping != NULL) *out_mapping = mapping;
		else mapping_release(mapping);
	}
	return mapped;
}
