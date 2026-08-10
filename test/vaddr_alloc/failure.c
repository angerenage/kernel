#include <core/pmm.h>

#include "test_support.h"

Test(vaddr_alloc, failed_reservations_are_side_effect_free) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct address_space*  space;
	size_t                 initial_free;
	uintptr_t              out;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x40000000ull, 32u);
	space        = address_space_kernel();
	initial_free = address_space_free_page_count(space);

	out = (uintptr_t)-1;
	cr_assert_not(address_space_reserve(space, 0u, 1u, &out), "zero-page reservation must fail");
	cr_assert_eq(out, 0u, "failed reservation must clear its output base");
	cr_assert_eq(address_space_free_page_count(space), initial_free, "zero-page failure must not change accounting");

	out = (uintptr_t)-1;
	cr_assert_not(address_space_reserve(space, 1u, 3u, &out), "non-power-of-two alignment must fail");
	cr_assert_eq(out, 0u, "invalid-alignment failure must clear its output base");
	cr_assert_eq(
		address_space_free_page_count(space), initial_free, "invalid-alignment failure must not change accounting");

	out = (uintptr_t)-1;
	cr_assert_not(address_space_reserve(space, initial_free + 1u, 1u, &out), "oversized reservation must fail");
	cr_assert_eq(out, 0u, "oversized failure must clear its output base");
	cr_assert_eq(address_space_free_page_count(space), initial_free, "oversized failure must not change accounting");
}

Test(vaddr_alloc, failed_release_is_atomic_across_mixed_page_state) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct address_space*  space;
	const uintptr_t        base = 0x50000000ull;
	size_t                 initial_free;

	init_test_vaddr_alloc(arena, sizeof(arena), base, 16u);
	space        = address_space_kernel();
	initial_free = address_space_free_page_count(space);

	cr_assert(address_space_reserve_at(space, base, 1u), "first exact reservation failed");
	cr_assert(address_space_reserve_at(space, base + 2u * PMM_PAGE_SIZE, 1u), "second exact reservation failed");
	cr_assert_eq(address_space_free_page_count(space), initial_free - 2u, "reservation accounting mismatch");

	cr_assert_not(address_space_release(space, base, 3u), "release spanning an unreserved page must fail");
	cr_assert_eq(address_space_free_page_count(space),
	             initial_free - 2u,
	             "failed mixed-state release must not partially free pages");

	cr_assert_not(address_space_reserve_at(space, base, 1u), "first page must remain reserved after failed release");
	cr_assert_not(address_space_reserve_at(space, base + 2u * PMM_PAGE_SIZE, 1u),
	              "last page must remain reserved after failed release");
}

Test(vaddr_alloc, failed_reinitialization_preserves_the_existing_allocator) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct address_space*  space;
	const uintptr_t        base        = 0x60000000ull;
	uintptr_t              reservation = 0u;
	uintptr_t              bitmap_phys;
	size_t                 free_before;

	init_test_vaddr_alloc(arena, sizeof(arena), base, 32u);
	space = address_space_kernel();

	cr_assert(address_space_reserve(space, 4u, 1u, &reservation), "initial reservation failed");
	bitmap_phys = space->bitmap_phys;
	free_before = address_space_free_page_count(space);

	cr_assert_not(address_space_init(space, base + 1u, 32u), "misaligned reinitialization must fail");
	cr_assert(address_space_is_initialized(space), "failed reinitialization must not destroy the existing allocator");
	cr_assert_eq(space->base, base, "failed reinitialization must preserve the original base");
	cr_assert_eq(
		space->bitmap_phys, bitmap_phys, "failed reinitialization must preserve the existing bitmap allocation");
	cr_assert_eq(address_space_total_page_count(space), 32u, "failed reinitialization must preserve total capacity");
	cr_assert_eq(address_space_free_page_count(space),
	             free_before,
	             "failed reinitialization must preserve allocation accounting");

	cr_assert(address_space_release(space, reservation, 4u),
	          "reservation made before failed reinitialization must remain valid");
	cr_assert_eq(address_space_free_page_count(space),
	             32u,
	             "releasing the preserved reservation must restore the original capacity");
}
