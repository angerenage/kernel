#include "test_support.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <core/vm_space.h>

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
	cr_assert(vm_init(), "vm_init failed");
}

size_t vmm_test_pages_consumed_since(size_t free_before) {
	size_t free_after = pmm_free_page_count();
	return free_before >= free_after ? free_before - free_after : 0u;
}

bool test_vm_map(struct address_space* space, size_t page_count, vmm_prot_t prot, uintptr_t requested_base,
                 size_t align_pages, size_t guard_pages, vmm_id_t* out_id, void** out_base) {
	struct memory_object* memory;
	if (!memory_object_create_owned(page_count, &memory)) return false;
	bool mapped = vm_space_map(space,
	                           &(const struct vm_map_request){
								   .memory         = memory,
								   .page_count     = page_count,
								   .requested_base = requested_base,
								   .align_pages    = align_pages,
								   .guard_pages    = guard_pages,
								   .prot           = prot,
							   },
	                           out_id,
	                           out_base);
	memory_object_release(memory);
	return mapped;
}
