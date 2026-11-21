#include <stddef.h>
#include <stdint.h>

#include <core/memory.h>
#include <core/mm.h>
#include <core/pmm.h>

#include "../selftest.h"

static inline volatile uint64_t* kernel_selftest_pmm_phys_to_virt(uintptr_t phys) {
	return (volatile uint64_t*)(uintptr_t)(phys + mm_info.direct_map_offset);
}

static void kernel_selftest_pmm_allocates_contiguous_extents_and_restores_state(struct kernel_selftest_context* ctx) {
	const struct pmm_info* info = pmm_info();
	struct pmm_extent run = {0};
	size_t free_before = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, info != NULL, "pmm reported no allocation properties", cleanup);
	free_before = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_MSG(ctx, pmm_managed_range_count() > 0u, "pmm reported no managed ranges");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, pmm_total_size() > 0u, "pmm reported no managed memory");
	KERNEL_SELFTEST_ASSERT(ctx, free_before >= 3u * info->allocation_granule);
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= pmm_total_size());
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = 3u * info->allocation_granule, .alignment = info->allocation_granule
					},
					&run),
			"three-granule PMM allocation failed",
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (run.address & (info->allocation_granule - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, run.size == 3u * info->allocation_granule, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_size() == free_before - run.size, cleanup);

	*kernel_selftest_pmm_phys_to_virt(run.address) = 0x1122334455667788ull;
	*kernel_selftest_pmm_phys_to_virt(run.address + 2u * info->allocation_granule) = 0x8877665544332211ull;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, *kernel_selftest_pmm_phys_to_virt(run.address) == 0x1122334455667788ull, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			*kernel_selftest_pmm_phys_to_virt(run.address + 2u * info->allocation_granule) == 0x8877665544332211ull,
			cleanup);

	struct pmm_extent released = run;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm_free(run), "PMM extent release failed", cleanup);
	run = (struct pmm_extent){0};
	KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
	KERNEL_SELFTEST_ASSERT(ctx, !pmm_free(released));

cleanup:
	if (run.size != 0u) (void)pmm_free(run);
	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
}

static void kernel_selftest_pmm_reuses_freed_extents(struct kernel_selftest_context* ctx) {
	const struct pmm_info* info = pmm_info();
	struct pmm_extent first = {0}, reused = {0};
	size_t free_before = 0u;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info != NULL, cleanup);
	free_before = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = info->allocation_granule, .alignment = info->allocation_granule
					},
					&first),
			"single-granule PMM allocation failed",
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free(first), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = info->allocation_granule, .alignment = info->allocation_granule
					},
					&reused),
			"replacement PMM allocation failed",
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reused.address == first.address, cleanup);
	first = (struct pmm_extent){0};

cleanup:
	if (reused.size != 0u) (void)pmm_free(reused);
	else if (first.size != 0u) (void)pmm_free(first);
	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
}

static void kernel_selftest_pmm_rejects_invalid_requests(struct kernel_selftest_context* ctx) {
	const struct pmm_info* info = pmm_info();
	struct pmm_extent extent = {0};
	size_t free_before = 0u;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info != NULL, cleanup);
	free_before = pmm_free_size();
	KERNEL_SELFTEST_ASSERT(
			ctx, !pmm_alloc(&(const struct pmm_alloc_request){.alignment = info->allocation_granule}, &extent));
	KERNEL_SELFTEST_ASSERT(ctx,
			!pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = info->allocation_granule, .alignment = info->allocation_granule
					},
					NULL));
	KERNEL_SELFTEST_ASSERT(ctx, !pmm_free((struct pmm_extent){.address = 1u, .size = info->allocation_granule}));
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = info->allocation_granule, .alignment = info->allocation_granule
					},
					&extent),
			cleanup);
	{
		struct pmm_extent overflowing = {
			.address = extent.address,
			.size = (size_t)(UINTPTR_MAX - extent.address) + 1u,
		};
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_claim(overflowing) == PMM_CLAIM_UNAVAILABLE, cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, !pmm_free(overflowing), cleanup);
	}
	KERNEL_SELFTEST_ASSERT_GOTO(
			ctx, !pmm_free((struct pmm_extent){.address = extent.address + 1u, .size = extent.size}), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free(extent), cleanup);
	extent = (struct pmm_extent){0};
	KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);

cleanup:
	if (extent.size != 0u) (void)pmm_free(extent);
	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
}

static void kernel_selftest_pmm_fixed_memory_materialization_enforces_constraints(struct kernel_selftest_context* ctx) {
	const struct pmm_info* info = pmm_info();
	struct pmm_extent extent = {0};
	struct memory* memory = NULL;
	size_t free_before = 0u;
	size_t size = 0u;
	uintptr_t address = 0u;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info != NULL, cleanup);
	free_before = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			pmm_alloc(
					&(const struct pmm_alloc_request){
						.size = 2u * info->allocation_granule,
						.alignment = 2u * info->allocation_granule,
					},
					&extent),
			"fixed-memory test extent allocation failed",
			cleanup);
	address = extent.address;
	size = extent.size;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free(extent), cleanup);
	extent = (struct pmm_extent){0};
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			memory_create_physical(
					&(const struct memory_physical_request){
						.physical_address = address,
						.size = size,
						.memory_type = MEMORY_TYPE_NORMAL,
					},
					&memory),
			"fixed-memory creation failed",
			cleanup);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			memory_materialize(memory,
					&(const struct memory_materialize_request){
						.size = size,
						.alignment = 2u * info->allocation_granule,
						.minimum_address = address,
						.maximum_address = address + size,
						.require_contiguous = true,
					}),
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			!memory_materialize(memory,
					&(const struct memory_materialize_request){
						.size = size,
						.alignment = info->allocation_granule,
						.minimum_address = address + info->allocation_granule,
					}),
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			!memory_materialize(memory,
					&(const struct memory_materialize_request){
						.size = size,
						.alignment = info->allocation_granule,
						.maximum_address = address + size - info->allocation_granule,
					}),
			cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
			!memory_materialize(memory,
					&(const struct memory_materialize_request){
						.offset = info->allocation_granule,
						.size = info->allocation_granule,
						.alignment = 2u * info->allocation_granule,
					}),
			cleanup);

cleanup:
	if (memory != NULL) memory_release(memory);
	else if (extent.size != 0u) (void)pmm_free(extent);
	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
}

static const struct kernel_selftest_case kernel_pmm_selftests[] = {
	{.name = "allocates_contiguous_extents_and_restores_state",
		.run = kernel_selftest_pmm_allocates_contiguous_extents_and_restores_state},
	{.name = "reuses_freed_extents", .run = kernel_selftest_pmm_reuses_freed_extents},
	{.name = "rejects_invalid_requests", .run = kernel_selftest_pmm_rejects_invalid_requests},
	{.name = "fixed_memory_materialization_enforces_constraints",
		.run = kernel_selftest_pmm_fixed_memory_materialization_enforces_constraints},
};

const struct kernel_selftest_suite kernel_pmm_selftest_suite = {
	.name = "pmm",
	.cases = kernel_pmm_selftests,
	.case_count = sizeof(kernel_pmm_selftests) / sizeof(kernel_pmm_selftests[0]),
};
