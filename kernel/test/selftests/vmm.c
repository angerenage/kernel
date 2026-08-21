#include <core/memory_object.h>
#include <core/pmm.h>
#include <core/vm_space.h>
#include <hal/paging.h>

#include "../selftest.h"

static void kernel_selftest_vmm_demand_maps_and_releases(struct kernel_selftest_context* ctx) {
	struct memory_object* memory = NULL;
	vmm_id_t              id     = VMM_ID_INVALID;
	void*                 base   = NULL;
	struct vmm_info       info;
	uint64_t              flags;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, memory_object_create_owned(2u, &memory), "object create failed", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                vm_space_map(vm_space_kernel(),
	                                             &(const struct vm_map_request){
													 .memory      = memory,
													 .page_count  = 2u,
													 .align_pages = 2u,
													 .guard_pages = 1u,
													 .prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
												 },
	                                             &id,
	                                             &base),
	                                "mapping create failed",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, base != NULL && id != VMM_ID_INVALID, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, !hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, !vm_space_query(vm_space_kernel(), (uintptr_t)base - PMM_PAGE_SIZE, &info), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_WRITE),
		"fault resolution failed",
		cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL, &flags), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (flags & HAL_PAGE_WRITE) != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vm_space_protect(vm_space_kernel(), id, VMM_PROT_READ | VMM_PROT_GLOBAL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL, &flags), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (flags & HAL_PAGE_WRITE) == 0u, cleanup);

cleanup:
	if (id != VMM_ID_INVALID) (void)vm_space_unmap(vm_space_kernel(), id);
	memory_object_release(memory);
}

static const struct kernel_selftest_case kernel_vmm_selftests[] = {
	{.name = "demand_maps_and_releases", .run = kernel_selftest_vmm_demand_maps_and_releases},
};

const struct kernel_selftest_suite kernel_vmm_selftest_suite = {
	.name       = "vmm",
	.cases      = kernel_vmm_selftests,
	.case_count = sizeof(kernel_vmm_selftests) / sizeof(kernel_vmm_selftests[0]),
};
