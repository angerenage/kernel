#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/pmm.h>
#include <core/vm_space.h>
#include <hal/paging.h>

#include "../selftest.h"

static void kernel_selftest_vmm_demand_maps_and_releases(struct kernel_selftest_context* ctx) {
	struct memory_object*         memory = NULL;
	vmm_id_t                      id     = VMM_ID_INVALID;
	void*                         base   = NULL;
	struct vmm_info               info;
	struct hal_paging_translation translation;

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
		ctx, !hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, !vm_space_query(vm_space_kernel(), (uintptr_t)base - VMM_PAGE_SIZE, &info), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_WRITE),
		"fault resolution failed",
		cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (translation.flags & HAL_PAGE_WRITE) != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, vm_space_protect(vm_space_kernel(), id, VMM_PROT_READ | VMM_PROT_GLOBAL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (translation.flags & HAL_PAGE_WRITE) == 0u, cleanup);

cleanup:
	if (id != VMM_ID_INVALID) (void)vm_space_unmap(vm_space_kernel(), id);
	memory_object_release(memory);
}

static void kernel_selftest_vmm_large_leaf_split(struct kernel_selftest_context* ctx) {
	const struct hal_paging_info* paging   = hal_paging_info();
	struct hal_paging_space*      space    = NULL;
	uintptr_t                     physical = 0u;
	uintptr_t                     new_physical;
	struct pmm_extent             allocation = {0};
	size_t                        large_size = 0u;
	uintptr_t                     virtual    = 0u;
	struct hal_paging_translation translation;
	size_t                        page_count            = 0u;
	size_t                        allocation_page_count = 0u;
	size_t                        free_after_create     = 0u;
	bool                          mapped                = false;
	bool                          active                = false;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, paging != NULL, cleanup);
	for (unsigned shift = 0u; shift < sizeof(size_t) * 8u; shift++) {
		size_t candidate = (size_t)1u << shift;
		if (candidate > paging->minimum_leaf_size && (paging->leaf_size_mask & (1ull << shift)) != 0u) {
			large_size = candidate;
			break;
		}
	}
	if (large_size == 0u) return;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, large_size <= UINTPTR_MAX / 2u, cleanup);
	virtual    = (uintptr_t)large_size * 2u;
	page_count = large_size / VMM_PAGE_SIZE + (large_size % VMM_PAGE_SIZE != 0u ? 1u : 0u);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, page_count <= SIZE_MAX / 3u, cleanup);
	allocation_page_count = page_count * 3u;
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx,
		pmm_alloc(&(const struct pmm_alloc_request){.size      = allocation_page_count * VMM_PAGE_SIZE,
	                                                .alignment = VMM_PAGE_SIZE},
	              &allocation),
		cleanup);
	physical     = (allocation.address + large_size - 1u) & ~(uintptr_t)(large_size - 1u);
	new_physical = physical + large_size;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_space_create(&space), cleanup);
	free_after_create = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            hal_paging_map(space,
	                                           &(const struct hal_paging_map_request){
												   .virtual_address  = virtual,
												   .physical_address = physical,
												   .size             = large_size,
												   .flags            = HAL_PAGE_READ | HAL_PAGE_WRITE,
												   .memory_type      = MEMORY_TYPE_NORMAL,
											   }),
	                            cleanup);
	mapped = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_activate(space), cleanup);
	active                                                         = true;
	*(volatile uint64_t*)virtual                                   = 0x123456789abcdef0ull;
	*(volatile uint64_t*)(virtual + paging->minimum_leaf_size)     = 0x1122334455667788ull;
	*(volatile uint64_t*)(virtual + large_size - sizeof(uint64_t)) = 0x0fedcba987654321ull;
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, *(volatile uint64_t*)(physical + boot_info.direct_map_offset) == 0x123456789abcdef0ull, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            *(volatile uint64_t*)(physical + large_size - sizeof(uint64_t) +
	                                                  boot_info.direct_map_offset) == 0x0fedcba987654321ull,
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_activate(hal_paging_kernel_space()), cleanup);
	active = false;
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(space, virtual + paging->minimum_leaf_size, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.physical_address == physical + paging->minimum_leaf_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.leaf_size == large_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            hal_paging_remap(space,
	                                             &(const struct hal_paging_remap_request){
													 .virtual_address  = virtual + paging->minimum_leaf_size,
													 .physical_address = new_physical + paging->minimum_leaf_size,
													 .size             = paging->minimum_leaf_size,
												 }),
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(space, virtual + paging->minimum_leaf_size, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.physical_address == new_physical + paging->minimum_leaf_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.leaf_size == paging->minimum_leaf_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.flags == (HAL_PAGE_READ | HAL_PAGE_WRITE), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.memory_type == MEMORY_TYPE_NORMAL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_activate(space), cleanup);
	active                                                     = true;
	*(volatile uint64_t*)(virtual + paging->minimum_leaf_size) = 0x8877665544332211ull;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_activate(hal_paging_kernel_space()), cleanup);
	active = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            *(volatile uint64_t*)(new_physical + paging->minimum_leaf_size +
	                                                  boot_info.direct_map_offset) == 0x8877665544332211ull,
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            *(volatile uint64_t*)(physical + paging->minimum_leaf_size +
	                                                  boot_info.direct_map_offset) == 0x1122334455667788ull,
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx,
		hal_paging_protect(space, virtual + paging->minimum_leaf_size, paging->minimum_leaf_size, HAL_PAGE_READ),
		cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(space, virtual + paging->minimum_leaf_size, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.leaf_size == paging->minimum_leaf_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (translation.flags & HAL_PAGE_WRITE) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_unmap(space, virtual + 2u * paging->minimum_leaf_size, paging->minimum_leaf_size), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_paging_query(space, virtual + 2u * paging->minimum_leaf_size, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            !hal_paging_remap(space,
	                                              &(const struct hal_paging_remap_request){
													  .virtual_address  = virtual + paging->minimum_leaf_size,
													  .physical_address = new_physical + 4u * paging->minimum_leaf_size,
													  .size             = 2u * paging->minimum_leaf_size,
												  }),
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_paging_query(space, virtual + paging->minimum_leaf_size, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.physical_address == new_physical + paging->minimum_leaf_size, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.flags == HAL_PAGE_READ, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.memory_type == MEMORY_TYPE_NORMAL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_unmap(space, virtual, large_size), cleanup);
	mapped = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_size() == free_after_create, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            hal_paging_map(space,
	                                           &(const struct hal_paging_map_request){
												   .virtual_address  = virtual,
												   .physical_address = physical,
												   .size             = paging->minimum_leaf_size,
												   .flags            = HAL_PAGE_READ | HAL_PAGE_WRITE,
												   .memory_type      = MEMORY_TYPE_NORMAL,
											   }),
	                            cleanup);
	mapped             = true;
	size_t free_before = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx,
	                            !hal_paging_map(space,
	                                            &(const struct hal_paging_map_request){
													.virtual_address = virtual - large_size + paging->minimum_leaf_size,
													.physical_address = physical + paging->minimum_leaf_size,
													.size             = large_size,
													.flags            = HAL_PAGE_READ | HAL_PAGE_WRITE,
													.memory_type      = MEMORY_TYPE_NORMAL,
												}),
	                            cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free_size() == free_before, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, !hal_paging_query(space, virtual - large_size + paging->minimum_leaf_size, NULL), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_paging_query(space, virtual, &translation), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, translation.physical_address == physical, cleanup);

cleanup:
	if (active) (void)hal_paging_activate(hal_paging_kernel_space());
	if (mapped) (void)hal_paging_unmap(space, virtual, large_size);
	if (space != NULL) hal_paging_space_destroy(space);
	if (allocation.size != 0u) (void)pmm_free(allocation);
}

static const struct kernel_selftest_case kernel_vmm_selftests[] = {
	{.name = "demand_maps_and_releases", .run = kernel_selftest_vmm_demand_maps_and_releases},
	{		.name = "large_leaf_split",         .run = kernel_selftest_vmm_large_leaf_split},
};

const struct kernel_selftest_suite kernel_vmm_selftest_suite = {
	.name       = "vmm",
	.cases      = kernel_vmm_selftests,
	.case_count = sizeof(kernel_vmm_selftests) / sizeof(kernel_vmm_selftests[0]),
};
