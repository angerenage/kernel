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
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(address_space_init(), "address_space_init failed");
}

size_t vmm_test_bytes_consumed_since(size_t free_before) {
	size_t free_after = pmm_free_size();
	return free_before >= free_after ? free_before - free_after : 0u;
}

bool test_vm_map(struct address_space* space, size_t page_count, vmm_prot_t prot, uintptr_t requested_base,
                 size_t align_pages, size_t guard_pages, struct mapping** out_mapping, void** out_base) {
	struct memory*  memory;
	struct mapping* mapping;
	if (page_count > SIZE_MAX / VMM_PAGE_SIZE || !memory_create_anonymous(page_count * VMM_PAGE_SIZE, &memory))
		return false;
	mapping_access_t access = 0u;
	if ((prot & VMM_PROT_READ) != 0u) access |= MAPPING_ACCESS_READ;
	if ((prot & VMM_PROT_WRITE) != 0u) access |= MAPPING_ACCESS_WRITE;
	if ((prot & VMM_PROT_EXEC) != 0u) access |= MAPPING_ACCESS_EXEC;
	bool mapped = address_space_map(space,
	                                &(const struct address_space_mapping_request){
										.memory       = memory,
										.address      = requested_base,
										.alignment    = (align_pages == 0u ? 1u : align_pages) * VMM_PAGE_SIZE,
										.guard_before = guard_pages * VMM_PAGE_SIZE,
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
