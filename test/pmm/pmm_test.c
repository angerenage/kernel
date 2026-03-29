#include "pmm_test.h"

bool phys_in_range(uintptr_t phys, uintptr_t base, size_t length) {
	return phys >= base && phys < base + length;
}

void init_test_pmm(uint8_t* arena, size_t arena_size) {
	struct limine_memmap_entry usable_low = {
		.base   = (uint64_t)(uintptr_t)(arena + PMM_TEST_LOW_OFFSET),
		.length = (uint64_t)PMM_TEST_LOW_LENGTH,
		.type   = LIMINE_MEMMAP_USABLE,
	};
	struct limine_memmap_entry reserved_gap = {
		.base   = (uint64_t)(uintptr_t)(arena + KiB(32)),
		.length = (uint64_t)KiB(8),
		.type   = LIMINE_MEMMAP_RESERVED,
	};
	struct limine_memmap_entry usable_high = {
		.base   = (uint64_t)(uintptr_t)(arena + PMM_TEST_HIGH_OFFSET),
		.length = (uint64_t)PMM_TEST_HIGH_LENGTH,
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
	cr_assert(early_init(&resp, 0), "early_init failed");
	cr_assert(pmm_init(), "pmm_init failed");
}
