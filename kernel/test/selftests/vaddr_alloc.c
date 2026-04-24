#include <core/pmm.h>
#include <core/vaddr_alloc.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"

static void kernel_selftest_vaddr_alloc_reserves_releases_and_reuses_ranges(struct kernel_selftest_context* ctx) {
	uintptr_t first       = 0;
	uintptr_t second      = 0;
	uintptr_t reused      = 0;
	size_t    free_before = address_space_free_page_count(address_space_kernel());
	size_t    total_pages = address_space_total_page_count(address_space_kernel());

	KERNEL_SELFTEST_ASSERT_MSG(
		ctx, address_space_is_initialized(address_space_kernel()), "kernel address space is not initialized");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_pages > 0u, "vaddr allocator reported zero pages");
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= total_pages);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                address_space_reserve(address_space_kernel(), 2u, 1u, &first),
	                                "address_space_reserve first range failed",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, first != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, address_space_free_page_count(address_space_kernel()) == free_before - 2u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                address_space_reserve(address_space_kernel(), 4u, 4u, &second),
	                                "address_space_reserve aligned range failed",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, second != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, second != first, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (second & ((uintptr_t)4u * (uintptr_t)PMM_PAGE_SIZE - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, address_space_free_page_count(address_space_kernel()) == free_before - 6u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                address_space_release(address_space_kernel(), first, 2u),
	                                "address_space_release first range failed",
	                                cleanup);
	first = 0u;
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, address_space_free_page_count(address_space_kernel()) == free_before - 4u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                address_space_reserve(address_space_kernel(), 2u, 1u, &reused),
	                                "address_space_reserve did not reuse a released range",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reused != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, address_space_free_page_count(address_space_kernel()) == free_before - 6u, cleanup);

cleanup:
	if (reused != 0u) (void)address_space_release(address_space_kernel(), reused, 2u);
	if (second != 0u) (void)address_space_release(address_space_kernel(), second, 4u);
	if (first != 0u) (void)address_space_release(address_space_kernel(), first, 2u);

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, address_space_free_page_count(address_space_kernel()) == free_before);
	}
}

static const struct kernel_selftest_case kernel_vaddr_alloc_selftests[] = {
	{
     .name = "reserves_releases_and_reuses_ranges",
     .run  = kernel_selftest_vaddr_alloc_reserves_releases_and_reuses_ranges,
	 },
};

const struct kernel_selftest_suite kernel_vaddr_alloc_selftest_suite = {
	.name       = "vaddr_alloc",
	.cases      = kernel_vaddr_alloc_selftests,
	.case_count = sizeof(kernel_vaddr_alloc_selftests) / sizeof(kernel_vaddr_alloc_selftests[0]),
};
