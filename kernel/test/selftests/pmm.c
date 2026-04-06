#include <core/mm.h>
#include <core/pmm.h>
#include <kernel/selftest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline volatile uint64_t* kernel_selftest_pmm_phys_to_virt(uintptr_t phys) {
	return (volatile uint64_t*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static void kernel_selftest_pmm_allocates_contiguous_runs_and_restores_state(struct kernel_selftest_context* ctx) {
	size_t    free_before;
	size_t    total_pages;
	uintptr_t run       = 0;
	uintptr_t saved_run = 0;

	free_before = pmm_free_page_count();
	total_pages = pmm_total_page_count();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, pmm_managed_range_count() > 0u, "pmm reported no managed ranges");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_pages > 0u, "pmm reported no managed pages");
	KERNEL_SELFTEST_ASSERT(ctx, free_before >= 3u);
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= total_pages);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm_alloc_pages(3, &run), "pmm_alloc_pages(3) returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (run & (PMM_PAGE_SIZE - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_page_count() == free_before - 3u, cleanup);

	*kernel_selftest_pmm_phys_to_virt(run)                                 = 0x1122334455667788ull;
	*kernel_selftest_pmm_phys_to_virt(run + 2u * (uintptr_t)PMM_PAGE_SIZE) = 0x8877665544332211ull;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, *kernel_selftest_pmm_phys_to_virt(run) == 0x1122334455667788ull, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, *kernel_selftest_pmm_phys_to_virt(run + 2u * (uintptr_t)PMM_PAGE_SIZE) == 0x8877665544332211ull, cleanup);

	saved_run = run;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm_free_pages(run, 3), "pmm_free_pages(run, 3) returned false", cleanup);
	run = 0;

	KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
	KERNEL_SELFTEST_ASSERT(ctx, !pmm_free_pages(saved_run, 3));

cleanup:
	if (run != 0) (void)pmm_free_pages(run, 3);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
}

static void kernel_selftest_pmm_reuses_freed_pages(struct kernel_selftest_context* ctx) {
	size_t    free_before;
	uintptr_t first  = 0;
	uintptr_t reused = 0;
	bool      first_live;

	free_before = pmm_free_page_count();
	first_live  = false;

	KERNEL_SELFTEST_ASSERT_MSG(ctx, free_before > 0u, "pmm reported no free pages");
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm_alloc_pages(1, &first), "pmm_alloc_pages(1) returned false", cleanup);
	first_live = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_page_count() == free_before - 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm_free_pages(first, 1), "pmm_free_pages(first, 1) returned false", cleanup);
	first_live = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_page_count() == free_before, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, pmm_alloc_pages(1, &reused), "second pmm_alloc_pages(1) returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reused == first, cleanup);

cleanup:
	if (reused != 0) {
		(void)pmm_free_pages(reused, 1);
	}
	else if (first_live) {
		(void)pmm_free_pages(first, 1);
	}

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
}

static const struct kernel_selftest_case kernel_pmm_selftests[] = {
	{
     .name = "allocates_contiguous_runs_and_restores_state",
     .run  = kernel_selftest_pmm_allocates_contiguous_runs_and_restores_state,
	 },
	{
     .name = "reuses_freed_pages",
     .run  = kernel_selftest_pmm_reuses_freed_pages,
	 },
};

const struct kernel_selftest_suite kernel_pmm_selftest_suite = {
	.name       = "pmm",
	.cases      = kernel_pmm_selftests,
	.case_count = sizeof(kernel_pmm_selftests) / sizeof(kernel_pmm_selftests[0]),
};
