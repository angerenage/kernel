#include <core/pmm.h>
#include <core/vmm.h>
#include <hal/paging.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"

static void kernel_selftest_vmm_allocates_queries_and_frees_mapped_ranges(struct kernel_selftest_context* ctx) {
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 4,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id     = VMM_ID_INVALID;
	void*           base         = NULL;
	uintptr_t       phys         = 0;
	uint64_t        flags        = 0;
	size_t          free_before  = pmm_free_page_count();
	size_t          count_before = vmm_count();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, vmm_is_initialized(), "vmm is not initialized");
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, vmm_alloc(&params, &alloc_id, &base), "vmm_alloc returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, alloc_id != VMM_ID_INVALID, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, base != NULL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, (((uintptr_t)base) & ((uintptr_t)(params.align_pages * PMM_PAGE_SIZE) - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vmm_count() == count_before + 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, vmm_query_id(alloc_id, &info), "vmm_query_id returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.base == base, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.page_count == params.page_count, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.kind == params.kind, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.prot == params.prot, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.state == VMM_STATE_MAPPED, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                vmm_query((uint8_t*)base + PMM_PAGE_SIZE, &info),
	                                "vmm_query returned false for an interior address",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.id == alloc_id, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                hal_paging_query((uintptr_t)base, &phys, &flags),
	                                "hal_paging_query returned false for the mapped base",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (phys & (PMM_PAGE_SIZE - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, flags == (uint64_t)(VMM_PROT_WRITE | VMM_PROT_GLOBAL), cleanup);

	((volatile uint8_t*)base)[0]             = 0x5au;
	((volatile uint8_t*)base)[PMM_PAGE_SIZE] = 0xa5u;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, ((volatile uint8_t*)base)[0] == 0x5au, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, ((volatile uint8_t*)base)[PMM_PAGE_SIZE] == 0xa5u, cleanup);

cleanup:
	if (alloc_id != VMM_ID_INVALID) (void)vmm_free(alloc_id);

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, vmm_count() == count_before);
		KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
	}
}

static void kernel_selftest_vmm_resolves_lazy_heap_faults_page_by_page(struct kernel_selftest_context* ctx) {
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id     = VMM_ID_INVALID;
	void*           base         = NULL;
	uint64_t        flags        = 0;
	size_t          free_before  = pmm_free_page_count();
	size_t          count_before = vmm_count();
	size_t          free_after_allocation;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_alloc(&params, &alloc_id, &base), "lazy vmm_alloc returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, alloc_id != VMM_ID_INVALID, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, base != NULL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vmm_count() == count_before + 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_query_id(alloc_id, &info), "vmm_query_id returned false for a lazy allocation", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.state == VMM_STATE_RESERVED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query((uintptr_t)base, NULL, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query((uintptr_t)base + PMM_PAGE_SIZE, NULL, NULL), cleanup);

	free_after_allocation = pmm_free_page_count();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_resolve_page_fault((uintptr_t)base + PMM_PAGE_SIZE), "vmm_resolve_page_fault returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query((uintptr_t)base, NULL, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_query((uintptr_t)base + PMM_PAGE_SIZE, NULL, &flags), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, flags == (uint64_t)VMM_PROT_WRITE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_page_count() < free_after_allocation, cleanup);
	((volatile uint8_t*)base)[PMM_PAGE_SIZE] = 0x33u;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, ((volatile uint8_t*)base)[PMM_PAGE_SIZE] == 0x33u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_query_id(alloc_id, &info), "vmm_query_id returned false after one lazy fault", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.state == VMM_STATE_PARTIAL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.first_phys == 0u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                vmm_resolve_page_fault((uintptr_t)base),
	                                "vmm_resolve_page_fault returned false for the first page",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_query((uintptr_t)base, NULL, NULL), cleanup);
	((volatile uint8_t*)base)[0] = 0x7cu;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, ((volatile uint8_t*)base)[0] == 0x7cu, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_query_id(alloc_id, &info), "vmm_query_id returned false after both lazy faults", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.state == VMM_STATE_MAPPED, cleanup);

cleanup:
	if (alloc_id != VMM_ID_INVALID) (void)vmm_free(alloc_id);

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, vmm_count() == count_before);
		KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
	}
}

static void kernel_selftest_vmm_keeps_stack_guard_pages_unmapped(struct kernel_selftest_context* ctx) {
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_STACK,
		.guard_pages = 1,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id     = VMM_ID_INVALID;
	void*           base         = NULL;
	size_t          free_before  = pmm_free_page_count();
	size_t          count_before = vmm_count();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_alloc(&params, &alloc_id, &base), "lazy stack vmm_alloc returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, alloc_id != VMM_ID_INVALID, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, base != NULL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vmm_count() == count_before + 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !vmm_query((uint8_t*)base - PMM_PAGE_SIZE, &info), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !vmm_resolve_page_fault((uintptr_t)base + PMM_PAGE_SIZE), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !vmm_resolve_page_fault((uintptr_t)base - PMM_PAGE_SIZE), cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                vmm_resolve_page_fault((uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE),
	                                "stack top fault was not resolved",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query((uintptr_t)base, NULL, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query((uintptr_t)base + PMM_PAGE_SIZE, NULL, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query((uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE, NULL, NULL), cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_resolve_page_fault((uintptr_t)base + PMM_PAGE_SIZE), "stack middle fault was not resolved", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_resolve_page_fault((uintptr_t)base), "stack base fault was not resolved", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, vmm_query_id(alloc_id, &info), "vmm_query_id returned false for the stack allocation", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.kind == VMM_KIND_STACK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.guard_pages == params.guard_pages, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.state == VMM_STATE_MAPPED, cleanup);

cleanup:
	if (alloc_id != VMM_ID_INVALID) (void)vmm_free(alloc_id);

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, vmm_count() == count_before);
		KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
	}
}

static void kernel_selftest_vmm_free_at_releases_tracking_and_backing(struct kernel_selftest_context* ctx) {
	struct vmm_alloc_params params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id     = VMM_ID_INVALID;
	void*           base         = NULL;
	size_t          free_before  = pmm_free_page_count();
	size_t          count_before = vmm_count();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, vmm_alloc(&params, &alloc_id, &base), "vmm_alloc returned false", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, alloc_id != VMM_ID_INVALID, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, base != NULL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vmm_query_id(alloc_id, &info), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, vmm_free_at(base), "vmm_free_at returned false", cleanup);
	alloc_id = VMM_ID_INVALID;
	base     = NULL;
	KERNEL_SELFTEST_ASSERT(ctx, !vmm_query_id(info.id, &info));
	KERNEL_SELFTEST_ASSERT(ctx, vmm_count() == count_before);
	KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);

cleanup:
	if (alloc_id != VMM_ID_INVALID) (void)vmm_free(alloc_id);

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, vmm_count() == count_before);
		KERNEL_SELFTEST_ASSERT(ctx, pmm_free_page_count() == free_before);
	}
}

static const struct kernel_selftest_case kernel_vmm_selftests[] = {
	{
     .name = "allocates_queries_and_frees_mapped_ranges",
     .run  = kernel_selftest_vmm_allocates_queries_and_frees_mapped_ranges,
	 },
	{
     .name = "resolves_lazy_heap_faults_page_by_page",
     .run  = kernel_selftest_vmm_resolves_lazy_heap_faults_page_by_page,
	 },
	{
     .name = "keeps_stack_guard_pages_unmapped",
     .run  = kernel_selftest_vmm_keeps_stack_guard_pages_unmapped,
	 },
	{
     .name = "free_at_releases_tracking_and_backing",
     .run  = kernel_selftest_vmm_free_at_releases_tracking_and_backing,
	 },
};

const struct kernel_selftest_suite kernel_vmm_selftest_suite = {
	.name       = "vmm",
	.cases      = kernel_vmm_selftests,
	.case_count = sizeof(kernel_vmm_selftests) / sizeof(kernel_vmm_selftests[0]),
};
