#include "test_support.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <core/vmm.h>

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
         .length = KiB(40),
         .type   = MEM_RANGE_USABLE,
		 },
	};

	cr_assert_geq(arena_size, KiB(128), "test arena is too small");
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
}
