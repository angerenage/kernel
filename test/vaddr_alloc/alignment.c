#include <core/pmm.h>

#include "test_support.h"

Test(vaddr_alloc, honors_alignment_and_coalesces_on_free) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              prefix  = 0;
	uintptr_t              aligned = 0;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x20000000ull, 128);

	cr_assert(address_space_reserve(address_space_kernel(), 3, 1, &prefix), "failed to reserve prefix");
	cr_assert(address_space_reserve(address_space_kernel(), 8, 8, &aligned), "failed to reserve aligned range");
	cr_assert_eq(aligned & ((uintptr_t)(8 * PMM_PAGE_SIZE) - 1u), 0, "allocation did not honor page alignment");

	cr_assert(address_space_release(address_space_kernel(), prefix, 3), "failed to release prefix");
	cr_assert(address_space_release(address_space_kernel(), aligned, 8), "failed to release aligned range");
	cr_assert_eq(address_space_free_page_count(address_space_kernel()),
	             address_space_total_page_count(address_space_kernel()),
	             "allocator did not coalesce free space");

	{
		uintptr_t whole = 0;

		cr_assert(address_space_reserve(address_space_kernel(), 128, 1, &whole), "failed to reserve coalesced region");
		cr_assert_eq(whole, (uintptr_t)0x20000000ull, "coalesced region base mismatch");
	}
}

Test(vaddr_alloc, alignment_is_applied_to_the_absolute_virtual_address) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	const uintptr_t        space_base = 0x10001000ull;
	const uintptr_t        alignment  = 8u * (uintptr_t)PMM_PAGE_SIZE;
	uintptr_t              aligned    = 0u;

	init_test_vaddr_alloc(arena, sizeof(arena), space_base, 64u);

	cr_assert(address_space_reserve(address_space_kernel(), 2u, 8u, &aligned), "aligned reservation failed");
	cr_assert_eq(aligned & (alignment - 1u),
	             0u,
	             "alignment must apply to the absolute virtual address, not the window-relative page index");
	cr_assert_geq(aligned, space_base, "aligned reservation escaped below the address-space window");
	cr_assert_lt(aligned,
	             space_base + 64u * (uintptr_t)PMM_PAGE_SIZE,
	             "aligned reservation escaped above the address-space window");

	cr_assert(address_space_reserve_at(address_space_kernel(), space_base, 1u),
	          "alignment padding must remain free and independently reservable");
}
