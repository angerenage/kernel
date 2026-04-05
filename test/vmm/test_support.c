#include "test_support.h"

#include <core/pmm.h>
#include <core/vmm.h>
#include <limine.h>

void init_test_vmm(uint8_t* arena, size_t arena_size) {
	struct limine_memmap_entry usable_low = {
		.base   = (uint64_t)(uintptr_t)arena,
		.length = (uint64_t)KiB(24),
		.type   = LIMINE_MEMMAP_USABLE,
	};
	struct limine_memmap_entry reserved_gap = {
		.base   = (uint64_t)(uintptr_t)(arena + KiB(32)),
		.length = (uint64_t)KiB(8),
		.type   = LIMINE_MEMMAP_RESERVED,
	};
	struct limine_memmap_entry usable_high = {
		.base   = (uint64_t)(uintptr_t)(arena + KiB(64)),
		.length = (uint64_t)KiB(40),
		.type   = LIMINE_MEMMAP_USABLE,
	};
	struct limine_memmap_entry* entries[] = {
		&usable_low,
		&reserved_gap,
		&usable_high,
	};
	struct limine_memmap_response resp = {
		.entry_count = sizeof(entries) / sizeof(entries[0]),
		.entries     = entries,
	};

	cr_assert_geq(arena_size, KiB(128), "test arena is too small");
	mock_paging_reset();
	cr_assert(pmm_init(&resp, 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
}
