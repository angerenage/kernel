#include "vaddr_alloc_test.h"

#include <core/pmm.h>

void init_test_vaddr_alloc(uint8_t* arena, size_t arena_size, uintptr_t base, size_t page_count) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)arena,
         .length = arena_size,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vaddr_alloc_init(base, page_count), "vaddr_alloc_init failed");
}
