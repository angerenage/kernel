#include "test_support.h"

#define PMM_MAP_TEST_MAX_PAGES 64u

static bool allocation_seen(const uintptr_t* pages, size_t page_count, uintptr_t phys) {
	for (size_t i = 0; i < page_count; i++) {
		if (pages[i] == phys) return true;
	}
	return false;
}

Test(pmm, overlapping_usable_ranges_never_return_the_same_physical_page_twice) {
	_Alignas(PMM_PAGE_SIZE) uint8_t arena[KiB(128)];
	const struct mem_range          memory_map[] = {
        {
         .base   = (uintptr_t)arena,
         .length = KiB(64),
         .type   = MEM_RANGE_USABLE,
         },
        {
         .base   = (uintptr_t)(arena + KiB(32)),
         .length = KiB(64),
         .type   = MEM_RANGE_USABLE,
         },
    };
	uintptr_t allocated[PMM_MAP_TEST_MAX_PAGES] = {0};
	size_t    allocated_count                   = 0u;

	/*
	 * Rejecting an overlapping map is a valid policy. If it is accepted,
	 * however, each physical page must still have a single ownership bit.
	 */
	if (!pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u)) return;

	for (;;) {
		uintptr_t phys = 0u;

		if (!pmm_alloc_pages(1u, &phys)) break;
		cr_assert_lt(allocated_count, PMM_MAP_TEST_MAX_PAGES, "unexpected allocation count");
		cr_assert_not(allocation_seen(allocated, allocated_count, phys),
		              "overlapping usable extents returned physical page 0x%zx twice",
		              (size_t)phys);
		allocated[allocated_count++] = phys;
	}
}

Test(pmm, overlapping_nonusable_ranges_are_rejected_or_excluded_from_allocation) {
	_Alignas(PMM_PAGE_SIZE) uint8_t arena[KiB(128)];
	const uintptr_t                 reserved_base = (uintptr_t)(arena + KiB(32));
	const size_t                    reserved_size = KiB(8);
	const struct mem_range          memory_map[]  = {
        {
         .base   = (uintptr_t)arena,
         .length = KiB(64),
         .type   = MEM_RANGE_USABLE,
         },
        {
         .base   = reserved_base,
         .length = reserved_size,
         .type   = MEM_RANGE_RESERVED,
         },
    };

	/* As above, rejecting an ambiguous map is safe; accepting it must honor the exclusion. */
	if (!pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u)) return;

	for (;;) {
		uintptr_t phys = 0u;

		if (!pmm_alloc_pages(1u, &phys)) break;
		cr_assert_not(phys_in_range(phys, reserved_base, reserved_size),
		              "PMM allocated page 0x%zx from an overlapping reserved extent",
		              (size_t)phys);
	}
}

Test(pmm, adjacent_usable_extents_do_not_artificially_break_contiguous_runs) {
	_Alignas(PMM_PAGE_SIZE) uint8_t arena[KiB(64)];
	const struct mem_range          memory_map[] = {
        {
         .base   = (uintptr_t)arena,
         .length = KiB(32),
         .type   = MEM_RANGE_USABLE,
         },
        {
         .base   = (uintptr_t)(arena + KiB(32)),
         .length = KiB(32),
         .type   = MEM_RANGE_USABLE,
         },
    };
	uintptr_t run = 0u;

	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u),
	          "adjacent usable extents must form a valid memory map");
	cr_assert(pmm_alloc_pages(9u, &run), "adjacent extents must allow a contiguous allocation crossing their boundary");
	cr_assert_lt(run, (uintptr_t)(arena + KiB(32)), "run must begin in the first extent");
	cr_assert_gt(run + 9u * PMM_PAGE_SIZE, (uintptr_t)(arena + KiB(32)), "run must continue into the adjacent extent");
}
