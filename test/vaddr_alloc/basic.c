#include <core/pmm.h>

#include "vaddr_alloc_test.h"

Test(vaddr_alloc, allocates_releases_and_reuses_ranges) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              first  = 0;
	uintptr_t              second = 0;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x10000000ull, 64);

	cr_assert_eq(vaddr_alloc_total_page_count(), 64, "unexpected total page count");
	cr_assert_eq(vaddr_alloc_free_page_count(), 64, "unexpected initial free page count");

	cr_assert(vaddr_alloc_reserve(4, 1, &first), "failed to reserve first range");
	cr_assert_eq(first, (uintptr_t)0x10000000ull, "first allocation base mismatch");
	cr_assert_eq(vaddr_alloc_free_page_count(), 60, "free page count mismatch after first allocation");

	cr_assert(vaddr_alloc_reserve(8, 1, &second), "failed to reserve second range");
	cr_assert_eq(second, first + 4 * (uintptr_t)PMM_PAGE_SIZE, "second allocation base mismatch");
	cr_assert_eq(vaddr_alloc_free_page_count(), 52, "free page count mismatch after second allocation");

	cr_assert(vaddr_alloc_release(first, 4), "failed to release first range");
	cr_assert_eq(vaddr_alloc_free_page_count(), 56, "free page count mismatch after release");

	{
		uintptr_t reused = 0;

		cr_assert(vaddr_alloc_reserve(4, 1, &reused), "failed to reuse released range");
		cr_assert_eq(reused, first, "allocator did not reuse the first gap");
	}
}
