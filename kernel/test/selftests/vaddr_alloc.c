#include <core/pmm.h>
#include <core/vaddr_alloc.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"

static void kernel_selftest_vaddr_alloc_reserves_releases_and_reuses_ranges(struct kernel_selftest_context* ctx) {
	uintptr_t first       = 0;
	uintptr_t second      = 0;
	uintptr_t reused      = 0;
	size_t    free_before = vaddr_alloc_free_page_count();
	size_t    total_pages = vaddr_alloc_total_page_count();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, vaddr_alloc_is_initialized(), "vaddr allocator is not initialized");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_pages > 0u, "vaddr allocator reported zero pages");
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= total_pages);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vaddr_alloc_reserve(2u, 1u, &first), "vaddr_alloc_reserve first range failed", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, first != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vaddr_alloc_free_page_count() == free_before - 2u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vaddr_alloc_reserve(4u, 4u, &second), "vaddr_alloc_reserve aligned range failed", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, second != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, second != first, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (second & ((uintptr_t)4u * (uintptr_t)PMM_PAGE_SIZE - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vaddr_alloc_free_page_count() == free_before - 6u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vaddr_alloc_release(first, 2u), "vaddr_alloc_release first range failed", cleanup);
	first = 0u;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vaddr_alloc_free_page_count() == free_before - 4u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vaddr_alloc_reserve(2u, 1u, &reused), "vaddr_alloc_reserve did not reuse a released range", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reused != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vaddr_alloc_free_page_count() == free_before - 6u, cleanup);

cleanup:
	if (reused != 0u) (void)vaddr_alloc_release(reused, 2u);
	if (second != 0u) (void)vaddr_alloc_release(second, 4u);
	if (first != 0u) (void)vaddr_alloc_release(first, 2u);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, vaddr_alloc_free_page_count() == free_before);
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
