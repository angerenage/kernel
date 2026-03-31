#include "vaddr_alloc_test.h"

#include <core/pmm.h>

void init_test_vaddr_alloc(uint8_t* arena, size_t arena_size, uintptr_t base, size_t page_count) {
	struct limine_memmap_entry entry = {
		.base   = (uint64_t)(uintptr_t)arena,
		.length = (uint64_t)arena_size,
		.type   = LIMINE_MEMMAP_USABLE,
	};
	struct limine_memmap_entry*   entries[] = {&entry};
	struct limine_memmap_response resp      = {
			 .entry_count = 1,
			 .entries     = entries,
    };

	cr_assert(pmm_init(&resp, 0), "pmm_init failed");
	cr_assert(vaddr_alloc_init(base, page_count), "vaddr_alloc_init failed");
}
