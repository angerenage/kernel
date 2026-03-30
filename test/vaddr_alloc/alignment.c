#include <core/pmm.h>

#include "vaddr_alloc_test.h"

Test(vaddr_alloc, honors_alignment_and_coalesces_on_free) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              prefix  = 0;
	uintptr_t              aligned = 0;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x20000000ull, 128);

	cr_assert(vaddr_alloc_reserve(3, 1, &prefix), "failed to reserve prefix");
	cr_assert(vaddr_alloc_reserve(8, 8, &aligned), "failed to reserve aligned range");
	cr_assert_eq(aligned & ((uintptr_t)(8 * PMM_PAGE_SIZE) - 1u), 0, "allocation did not honor page alignment");

	cr_assert(vaddr_alloc_release(prefix, 3), "failed to release prefix");
	cr_assert(vaddr_alloc_release(aligned, 8), "failed to release aligned range");
	cr_assert_eq(
		vaddr_alloc_free_page_count(), vaddr_alloc_total_page_count(), "allocator did not coalesce free space");

	{
		uintptr_t whole = 0;

		cr_assert(vaddr_alloc_reserve(128, 1, &whole), "failed to reserve coalesced region");
		cr_assert_eq(whole, (uintptr_t)0x20000000ull, "coalesced region base mismatch");
	}
}
