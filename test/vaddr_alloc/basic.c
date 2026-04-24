#include <core/pmm.h>

#include "vaddr_alloc_test.h"

Test(vaddr_alloc, allocates_releases_and_reuses_ranges) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	uintptr_t              first  = 0;
	uintptr_t              second = 0;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x10000000ull, 64);

	cr_assert_eq(address_space_total_page_count(address_space_kernel()), 64, "unexpected total page count");
	cr_assert_eq(address_space_free_page_count(address_space_kernel()), 64, "unexpected initial free page count");

	cr_assert(address_space_reserve(address_space_kernel(), 4, 1, &first), "failed to reserve first range");
	cr_assert_eq(first, (uintptr_t)0x10000000ull, "first allocation base mismatch");
	cr_assert_eq(
		address_space_free_page_count(address_space_kernel()), 60, "free page count mismatch after first allocation");

	cr_assert(address_space_reserve(address_space_kernel(), 8, 1, &second), "failed to reserve second range");
	cr_assert_eq(second, first + 4 * (uintptr_t)PMM_PAGE_SIZE, "second allocation base mismatch");
	cr_assert_eq(
		address_space_free_page_count(address_space_kernel()), 52, "free page count mismatch after second allocation");

	cr_assert(address_space_release(address_space_kernel(), first, 4), "failed to release first range");
	cr_assert_eq(address_space_free_page_count(address_space_kernel()), 56, "free page count mismatch after release");

	{
		uintptr_t reused = 0;

		cr_assert(address_space_reserve(address_space_kernel(), 4, 1, &reused), "failed to reuse released range");
		cr_assert_eq(reused, first, "allocator did not reuse the first gap");
	}
}

Test(vaddr_alloc, address_space_instances_reserve_independently) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct address_space   first_space  = {0};
	struct address_space   second_space = {0};
	uintptr_t              first        = 0;
	uintptr_t              second       = 0;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x10000000ull, 64);

	cr_assert(address_space_init(&first_space, 0x40000000ull, 16), "failed to initialize first address space");
	cr_assert(address_space_init(&second_space, 0x50000000ull, 16), "failed to initialize second address space");

	cr_assert(address_space_reserve(&first_space, 4, 1, &first), "failed to reserve from first address space");
	cr_assert(address_space_reserve(&second_space, 4, 1, &second), "failed to reserve from second address space");

	cr_assert_eq(first, (uintptr_t)0x40000000ull, "first address-space base mismatch");
	cr_assert_eq(second, (uintptr_t)0x50000000ull, "second address-space base mismatch");
	cr_assert_eq(address_space_free_page_count(&first_space), 12, "first free page count mismatch");
	cr_assert_eq(address_space_free_page_count(&second_space), 12, "second free page count mismatch");
	cr_assert_eq(
		address_space_free_page_count(address_space_kernel()), 64, "kernel address space was unexpectedly modified");

	address_space_deinit(&first_space);
	address_space_deinit(&second_space);
}
