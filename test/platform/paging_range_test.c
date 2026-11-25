#include <stdint.h>

#include <criterion/criterion.h>

#include "../../platforms/paging_range.h"

Test(paging_range, accepts_ranges_contained_in_either_canonical_half) {
	uintptr_t end = 0u;

	cr_assert(paging_virtual_range_end(0x4000u, 0x2000u, 48u, &end));
	cr_assert_eq(end, 0x6000u);
	cr_assert(paging_virtual_range_end(UINT64_C(0xffff800000004000), 0x2000u, 48u, &end));
	cr_assert_eq(end, UINT64_C(0xffff800000006000));
}

Test(paging_range, rejects_noncanonical_endpoints_and_gap_crossing) {
	uintptr_t end = 0u;

	cr_assert_not(paging_virtual_range_end(UINT64_C(0x0000800000000000), 0x1000u, 48u, &end));
	cr_assert_not(paging_virtual_range_end(UINT64_C(0x00007ffffffff000), 0x2000u, 48u, &end));
	cr_assert_not(paging_virtual_range_end(UINT64_C(0x00007ffffffff000), UINT64_C(0xffff000000002000), 48u, &end));
}

Test(paging_range, rejects_empty_overflowing_and_invalid_geometry) {
	uintptr_t end = 0u;

	cr_assert_not(paging_virtual_range_end(0x1000u, 0u, 48u, &end));
	cr_assert_not(paging_virtual_range_end(UINTPTR_MAX - 0xfffu, 0x2000u, 48u, &end));
	cr_assert_not(paging_virtual_range_end(0x1000u, 0x1000u, 0u, &end));
	cr_assert_not(paging_virtual_range_end(0x1000u, 0x1000u, 65u, &end));
	cr_assert_not(paging_virtual_range_end(0x1000u, 0x1000u, 48u, NULL));
}
