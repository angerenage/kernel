#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/pmm.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/cpu.h>
#include <hal/paging.h>
#include <string.h>

#include "test_support.h"

static size_t pages_consumed_since(size_t free_before) {
	size_t free_after = pmm_free_page_count();
	return free_before >= free_after ? (free_before - free_after) : 0;
}

Test(vmm, allocates_queries_and_frees_mapped_ranges) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 4,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       phys     = 0;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "vmm_alloc failed");
	cr_assert_neq(alloc_id, VMM_ID_INVALID, "vmm_alloc returned an invalid id");
	cr_assert_not_null(base, "vmm_alloc returned NULL base");
	cr_assert_eq(
		((uintptr_t)base) & ((uintptr_t)(4 * PMM_PAGE_SIZE) - 1u), 0, "vmm_alloc did not honor virtual alignment");
	cr_assert_eq(vmm_count(address_space_kernel()), 1, "vmm_count mismatch after alloc");
	cr_assert_eq(mock_paging_mapping_count(), 2, "mapped allocation did not create page mappings");
	cr_assert_geq(pages_consumed_since(free_before), 2, "mapped allocation did not consume backing pages");

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed");
	cr_assert_eq(info.base, base, "vmm_query_id returned the wrong base");
	cr_assert_eq(info.page_count, 2, "vmm_query_id returned the wrong size");
	cr_assert_eq(info.kind, VMM_KIND_HEAP, "vmm_query_id returned the wrong kind");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "vmm_query_id returned the wrong state");
	cr_assert_eq(info.prot, params.prot, "vmm_query_id returned the wrong protection");

	cr_assert(vmm_query(address_space_kernel(), (uint8_t*)base + PMM_PAGE_SIZE, &info),
	          "vmm_query failed for interior address");
	cr_assert_eq(info.id, alloc_id, "vmm_query returned the wrong allocation");

	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &phys, &flags),
	          "hal_paging_query failed for mapped allocation");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "mapped allocation physical address is not aligned");
	cr_assert_eq(flags, (uint64_t)(VMM_PROT_WRITE | VMM_PROT_GLOBAL), "mapped allocation flags mismatch");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "vmm_count mismatch after free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "vmm_free leaked physical pages");
}

Test(vmm, allocates_at_exact_addresses_and_rejects_overlaps) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        fixed_id  = VMM_ID_INVALID;
	vmm_id_t        mapped_id = VMM_ID_INVALID;
	uintptr_t       fixed     = vmm_window_base() + 8u * (uintptr_t)PMM_PAGE_SIZE;
	uintptr_t       mapped    = fixed + 4u * (uintptr_t)PMM_PAGE_SIZE;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc_at(address_space_kernel(), (void*)fixed, &params, &fixed_id),
	          "vmm_alloc_at failed for a fixed lazy range");
	cr_assert(vmm_query_id(address_space_kernel(), fixed_id, &info), "fixed allocation was not tracked");
	cr_assert_eq((uintptr_t)info.base, fixed, "fixed allocation base mismatch");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "fixed lazy allocation should start reserved");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fixed lazy allocation unexpectedly mapped pages");

	cr_assert(!vmm_alloc_at(address_space_kernel(), (void*)(fixed + PMM_PAGE_SIZE), &params, &mapped_id),
	          "vmm_alloc_at unexpectedly allowed an overlapping range");
	cr_assert_eq(mapped_id, VMM_ID_INVALID, "failed fixed allocation changed the id");

	params.map_flags = 0u;
	cr_assert(vmm_alloc_at(address_space_kernel(), (void*)mapped, &params, &mapped_id),
	          "vmm_alloc_at failed for a fixed eager range");
	cr_assert(vmm_query_id(address_space_kernel(), mapped_id, &info), "fixed eager allocation was not tracked");
	cr_assert_eq((uintptr_t)info.base, mapped, "fixed eager allocation base mismatch");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "fixed eager allocation should start mapped");
	cr_assert_eq(mock_paging_mapping_count(), 2, "fixed eager allocation did not map both pages");

	cr_assert(vmm_free(address_space_kernel(), mapped_id), "failed to free fixed eager allocation");
	cr_assert(vmm_free(address_space_kernel(), fixed_id), "failed to free fixed lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fixed allocations leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "fixed allocations leaked physical pages");
}

Test(vmm, supports_lazy_map_unmap_remap_and_reprotect) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       first_phys;
	uintptr_t       remapped_phys;
	uint64_t        flags;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert_eq(vmm_count(address_space_kernel()), 1, "vmm_count mismatch for lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation unexpectedly created mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation unexpectedly consumed physical pages");

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed for lazy allocation");
	cr_assert_eq(info.state, VMM_STATE_RESERVED, "lazy allocation did not start reserved");

	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed");
	cr_assert_eq(mock_paging_mapping_count(), 3, "vmm_map did not map every page");
	cr_assert_geq(pages_consumed_since(free_before), 3, "vmm_map did not consume physical pages");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &first_phys, &flags),
	          "hal_paging_query failed after vmm_map");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "vmm_map set incorrect initial flags");

	cr_assert(vmm_unmap(address_space_kernel(), alloc_id, false), "vmm_unmap(address_space_kernel(), false) failed");
	cr_assert_eq(mock_paging_mapping_count(), 0, "vmm_unmap(address_space_kernel(), false) leaked mappings");
	cr_assert_geq(pages_consumed_since(free_before),
	              3,
	              "vmm_unmap(address_space_kernel(), false) unexpectedly freed backing pages");

	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed when reusing preserved backing");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &remapped_phys, &flags),
	          "hal_paging_query failed after remap");
	cr_assert_eq(remapped_phys, first_phys, "vmm_map did not reuse preserved backing");

	cr_assert(vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL), "vmm_protect failed");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after vmm_protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_GLOBAL, "vmm_protect did not update page flags");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed for lazy allocation");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "vmm_count mismatch after lazy allocation free");
	cr_assert_eq(mock_paging_mapping_count(), 0, "lazy allocation free leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "lazy allocation free leaked physical pages");
}

Test(vmm, resolves_page_faults_for_lazy_heap_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uintptr_t       phys     = 0;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy reserved allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "fault resolution mapped more than the faulting page");
	cr_assert_lt(pages_consumed_since(free_before),
	             params.page_count + 1u,
	             "fault resolution allocated backing for the entire heap allocation");
	cr_assert(!hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, NULL),
	          "fault resolution unexpectedly mapped a non-faulting heap page");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base + PMM_PAGE_SIZE, NULL, &flags),
	          "fault resolution did not map the faulting address");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "fault resolution applied incorrect mapping flags");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + PMM_PAGE_SIZE),
	          "fault resolution unexpectedly retried an already-present heap page");
	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed after fault resolution");
	cr_assert_eq(info.state, VMM_STATE_PARTIAL, "fault resolution did not leave the heap allocation partially mapped");
	cr_assert_eq(info.first_phys, 0, "fault resolution unexpectedly reported backing for the untouched first page");

	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "vmm_resolve_page_fault failed for the first heap page");
	cr_assert_eq(mock_paging_mapping_count(), 2, "second fault resolution did not map the remaining heap page");
	cr_assert_geq(pages_consumed_since(free_before),
	              params.page_count,
	              "second fault resolution did not allocate the remaining heap backing");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, &phys, NULL),
	          "second fault resolution did not map the first heap page");
	cr_assert_eq(phys & (PMM_PAGE_SIZE - 1u), 0, "heap fault resolution returned an unaligned physical page");
	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info),
	          "vmm_query_id failed after resolving every heap page");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "heap allocation did not become fully mapped");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after fault resolution");
	cr_assert_eq(mock_paging_mapping_count(), 0, "fault resolution cleanup leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "fault resolution cleanup leaked physical pages");
}

Test(vmm, resolves_page_faults_for_lazy_stack_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_STACK,
		.guard_pages = 1,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy stack vmm_alloc failed");
	cr_assert(!vmm_query(address_space_kernel(), (uint8_t*)base - PMM_PAGE_SIZE, &info),
	          "stack guard page was exposed as a normal allocation");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + 1u * (uintptr_t)PMM_PAGE_SIZE),
	          "stack fault resolution unexpectedly skipped the top page");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy stack allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "stack fault resolution mapped more than the faulting page");
	cr_assert_lt(pages_consumed_since(free_before),
	             params.page_count + 1u,
	             "stack fault resolution allocated backing for the entire stack allocation");
	cr_assert(!hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, NULL),
	          "stack fault resolution unexpectedly mapped the base page");
	cr_assert(!hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base + PMM_PAGE_SIZE, NULL, NULL),
	          "stack fault resolution unexpectedly mapped the middle page");
	cr_assert(
		hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE, NULL, &flags),
		"stack fault resolution did not map the faulting address");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "stack fault resolution applied incorrect mapping flags");
	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info),
	          "vmm_query_id failed after stack fault resolution");
	cr_assert_eq(info.kind, VMM_KIND_STACK, "fault resolution changed the stack allocation kind");
	cr_assert_eq(info.guard_pages, params.guard_pages, "stack fault resolution changed guard page tracking");
	cr_assert_eq(
		info.state, VMM_STATE_PARTIAL, "stack fault resolution did not leave the stack allocation partially mapped");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base - PMM_PAGE_SIZE),
	          "stack fault resolution unexpectedly mapped the guard page");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "stack fault resolution unexpectedly skipped to the lowest usable page");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + PMM_PAGE_SIZE),
	          "stack fault resolution failed for the next growth page");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "stack fault resolution failed for the final usable page");
	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed after full stack growth");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "stack allocation did not become fully mapped");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after stack fault resolution");
	cr_assert_eq(mock_paging_mapping_count(), 0, "stack fault resolution cleanup leaked mappings");
	cr_assert_eq(pmm_free_page_count(), free_before, "stack fault resolution cleanup leaked physical pages");
}

Test(vmm, refuses_page_fault_resolution_for_non_lazy_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "eager vmm_alloc failed");
	cr_assert(vmm_unmap(address_space_kernel(), alloc_id, false),
	          "vmm_unmap(address_space_kernel(), false) failed for eager allocation");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base),
	          "vmm_resolve_page_fault unexpectedly remapped a non-lazy allocation");
	cr_assert_eq(mock_paging_mapping_count(), 0, "non-lazy fault resolution changed live mappings");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + KiB(128)),
	          "vmm_resolve_page_fault unexpectedly succeeded for an unknown address");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after rejected fault resolution");
	cr_assert_eq(pmm_free_page_count(), free_before, "rejected fault resolution leaked physical pages");
}

Test(vmm, current_page_fault_resolution_uses_thread_space_then_kernel_space) {
	_Alignas(4096) uint8_t   arena[KiB(512)];
	struct address_space     user_space = {0};
	struct hal_address_space user_hal_space;
	struct thread            current     = {0};
	struct vmm_alloc_params  user_params = {
		 .page_count  = 1,
		 .align_pages = 1,
		 .prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		 .kind        = VMM_KIND_HEAP,
		 .map_flags   = VMM_MAP_LAZY,
    };
	struct vmm_alloc_params kernel_params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t  user_id     = VMM_ID_INVALID;
	vmm_id_t  kernel_id   = VMM_ID_INVALID;
	void*     user_base   = NULL;
	void*     kernel_base = NULL;
	uintptr_t phys;
	uint64_t  flags;

	init_test_vmm(arena, sizeof(arena));
	hal_cpu_local_bind(NULL);
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cpu_bind_current(cpu_bsp());
	cr_assert(hal_paging_space_create(&user_hal_space), "hal_paging_space_create failed");
	cr_assert(address_space_init(&user_space, 0x40000000u, 16u), "address_space_init failed for user space");
	user_space.hal_space = user_hal_space;

	current.address_space         = &user_space;
	cpu_current()->current_thread = &current;

	cr_assert(vmm_alloc(&user_space, &user_params, &user_id, &user_base), "user lazy vmm_alloc failed");
	cr_assert(vmm_alloc(address_space_kernel(), &kernel_params, &kernel_id, &kernel_base),
	          "kernel lazy vmm_alloc failed");

	cr_assert(vmm_resolve_current_page_fault((uintptr_t)user_base),
	          "current fault resolution did not handle the user address space");
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)user_base, &phys, &flags),
	          "user fault did not map into the current thread address space");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "user fault used incorrect mapping flags");

	cr_assert(vmm_resolve_current_page_fault((uintptr_t)kernel_base),
	          "current fault resolution did not fall back to the kernel address space");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)kernel_base, &phys, &flags),
	          "kernel fallback fault did not map into the kernel address space");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_GLOBAL), "kernel fault used incorrect mapping flags");

	cpu_current()->current_thread = NULL;
	cr_assert(vmm_free(address_space_kernel(), kernel_id), "vmm_free failed for kernel allocation");
	cr_assert(vmm_free(&user_space, user_id), "vmm_free failed for user allocation");
	vmm_address_space_deinit(&user_space);
	hal_cpu_local_bind(NULL);
}

Test(vmm, rolls_back_partial_mappings_on_eager_map_failure) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	mock_paging_fail_after(1);

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "vmm_alloc unexpectedly succeeded");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed eager allocation left tracking metadata behind");
	cr_assert_eq(mock_paging_mapping_count(), 0, "rollback left mappings behind");
	cr_assert_eq(pmm_free_page_count(), free_before, "rollback leaked physical pages");
}

Test(vmm, rolls_back_failed_protect_and_preserves_original_mapping) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 3,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	struct vmm_info info;
	vmm_id_t        alloc_id = VMM_ID_INVALID;
	void*           base     = NULL;
	uint64_t        flags    = 0;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	cr_assert(vmm_map(address_space_kernel(), alloc_id), "vmm_map failed");

	mock_paging_fail_once_after(mock_paging_mapping_count() + 1u);
	cr_assert(!vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | VMM_PROT_GLOBAL),
	          "vmm_protect unexpectedly succeeded");

	for (size_t page = 0; page < params.page_count; page++) {
		uintptr_t virt = (uintptr_t)base + page * (uintptr_t)PMM_PAGE_SIZE;

		cr_assert(hal_paging_query(hal_paging_kernel_space(), virt, NULL, &flags),
		          "hal_paging_query failed after protect rollback");
		cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "protect rollback did not restore the original flags");
	}

	cr_assert(vmm_query_id(address_space_kernel(), alloc_id, &info), "vmm_query_id failed after protect rollback");
	cr_assert_eq(info.prot, params.prot, "failed protect changed the tracked protection");
	cr_assert_eq(info.state, VMM_STATE_MAPPED, "failed protect changed the tracked state");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after protect rollback");
	cr_assert_eq(mock_paging_mapping_count(), 0, "free leaked mappings after protect rollback");
	cr_assert_eq(pmm_free_page_count(), free_before, "free leaked physical pages after protect rollback");
}

Test(vmm, rejects_invalid_protection_masks) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params bad_params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | ((vmm_prot_t)1ull << 63),
		.kind        = VMM_KIND_GENERIC,
	};
	struct vmm_alloc_params good_params = {
		.page_count  = 1,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;
	uint64_t flags    = 0;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &bad_params, &alloc_id, &base),
	          "vmm_alloc accepted invalid protection bits");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed vmm_alloc changed the allocation id");
	cr_assert_null(base, "failed vmm_alloc changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed vmm_alloc left tracked allocations behind");

	cr_assert(vmm_alloc(address_space_kernel(), &good_params, &alloc_id, &base),
	          "vmm_alloc failed for valid protection bits");
	cr_assert(!vmm_protect(address_space_kernel(), alloc_id, VMM_PROT_READ | ((vmm_prot_t)1ull << 63)),
	          "vmm_protect accepted invalid protection bits");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after rejected protect");
	cr_assert_eq(flags, (uint64_t)VMM_PROT_WRITE, "rejected protect changed the live mapping");

	cr_assert(vmm_free(address_space_kernel(), alloc_id), "vmm_free failed after invalid protection tests");
}

Test(vmm, maps_and_reprotects_user_pages_distinct_from_kernel_pages) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 1,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
    };
	struct vmm_info           info;
	vmm_id_t                  alloc_id = VMM_ID_INVALID;
	void*                     base     = NULL;
	uint64_t                  flags    = 0;
	struct hal_address_space* user_hal_space;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base),
	          "kernel vmm_alloc accepted a user mapping");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed kernel user allocation changed the allocation id");
	cr_assert_null(base, "failed kernel user allocation changed the allocation base");

	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	user_hal_space = address_space_hal(&user_space);
	cr_assert_not_null(user_hal_space, "user address space did not create a HAL paging handle");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "vmm_alloc failed for a user mapping");
	cr_assert(vmm_query_id(&user_space, alloc_id, &info), "vmm_query_id failed for user mapping");
	cr_assert_eq(info.prot, params.prot, "vmm_query_id lost the user protection bit");
	cr_assert(!hal_paging_query(hal_paging_kernel_space(), (uintptr_t)base, NULL, NULL),
	          "user mapping leaked into the kernel address space");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed for user address-space mapping");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "user mapping flags were not translated");

	cr_assert(!vmm_protect(&user_space, alloc_id, VMM_PROT_READ | VMM_PROT_WRITE),
	          "vmm_protect allowed clearing user access inside a user address space");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after rejected protect");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "rejected protect changed user mapping flags");

	cr_assert(vmm_protect(&user_space, alloc_id, VMM_PROT_READ | VMM_PROT_EXEC | VMM_PROT_USER),
	          "vmm_protect failed to reprotect user access");
	cr_assert(hal_paging_query(user_hal_space, (uintptr_t)base, NULL, &flags),
	          "hal_paging_query failed after reprotecting user access");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_EXEC | HAL_PAGE_USER), "user exec mapping flags were not translated");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for user mapping");
	vmm_address_space_deinit(&user_space);
}

Test(vmm, validates_user_ranges_without_faulting_unless_requested) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 2,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "failed to allocate lazy user range");

	cr_assert_eq(
		address_space_validate_range(&user_space, (uintptr_t)base, 1u, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER),
		ADDRESS_TRANSFER_OK,
		"reserved user address should be valid by VMM metadata");
	cr_assert_eq(address_space_validate_range(&user_space,
	                                          (uintptr_t)base,
	                                          2u * PMM_PAGE_SIZE,
	                                          ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_PRESENT),
	             ADDRESS_TRANSFER_NOT_MAPPED,
	             "present validation should not fault in lazy pages");
	cr_assert_eq(
		address_space_validate_range(&user_space,
	                                 (uintptr_t)base,
	                                 2u * PMM_PAGE_SIZE,
	                                 ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN),
		ADDRESS_TRANSFER_OK,
		"fault-in validation should materialize lazy pages");
	cr_assert_eq(
		address_space_validate_range(
			&user_space, (uintptr_t)base + 2u * PMM_PAGE_SIZE, 1u, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER),
		ADDRESS_TRANSFER_NOT_MAPPED,
		"address after allocation should be rejected");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for user range");
	vmm_address_space_deinit(&user_space);
}

Test(vmm, copies_between_kernel_and_user_and_between_user_spaces) {
	_Alignas(4096) uint8_t arena[KiB(1024)];
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)arena,
         .length = sizeof(arena),
         .type   = MEM_RANGE_USABLE,
		 },
	};
	struct address_space    left_space  = {0};
	struct address_space    right_space = {0};
	struct vmm_alloc_params params      = {
			 .page_count  = 1,
			 .align_pages = 1,
			 .prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			 .kind        = VMM_KIND_GENERIC,
			 .map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t                     left_id    = VMM_ID_INVALID;
	vmm_id_t                     right_id   = VMM_ID_INVALID;
	void*                        left_base  = NULL;
	void*                        right_base = NULL;
	const char                   source[]   = "transfer-across-user-pages";
	char                         out[sizeof(source)];
	uintptr_t                    left_addr;
	uintptr_t                    right_addr;
	enum address_transfer_result transfer_result;

	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(address_space_init(&left_space, MM_USER_VMM_BASE, 16u), "failed to initialize left user address space");
	cr_assert(hal_paging_space_create(&left_space.hal_space), "failed to create left user HAL address space");
	cr_assert(address_space_init(&right_space, MM_USER_VMM_BASE, 16u), "failed to initialize right user address space");
	cr_assert(hal_paging_space_create(&right_space.hal_space), "failed to create right user HAL address space");
	cr_assert(vmm_alloc(&left_space, &params, &left_id, &left_base), "failed to allocate left user range");

	left_addr = (uintptr_t)left_base;
	transfer_result =
		address_space_validate_range(&left_space,
	                                 left_addr,
	                                 sizeof(source),
	                                 ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	cr_assert_eq(transfer_result, ADDRESS_TRANSFER_OK, "range fault-in failed with result %d", transfer_result);

	transfer_result = address_space_copy_to(&left_space, left_addr, source, sizeof(source));
	cr_assert_eq(transfer_result, ADDRESS_TRANSFER_OK, "kernel-to-user copy failed with result %d", transfer_result);
	memset(out, 0, sizeof(out));
	cr_assert_eq(address_space_copy_from(&left_space, left_addr, out, sizeof(out)),
	             ADDRESS_TRANSFER_OK,
	             "user-to-kernel copy failed");
	cr_assert_eq(memcmp(out, source, sizeof(source)), 0, "user-to-kernel copy returned wrong data");

	cr_assert(vmm_alloc(&right_space, &params, &right_id, &right_base), "failed to allocate right user range");
	right_addr = (uintptr_t)right_base;
	cr_assert_eq(address_space_copy_between(&right_space, right_addr, &left_space, left_addr, sizeof(source)),
	             ADDRESS_TRANSFER_OK,
	             "user-to-user copy failed");
	memset(out, 0, sizeof(out));
	cr_assert_eq(address_space_copy_from(&right_space, right_addr, out, sizeof(out)),
	             ADDRESS_TRANSFER_OK,
	             "right user-to-kernel copy failed");
	cr_assert_eq(memcmp(out, source, sizeof(source)), 0, "user-to-user copy returned wrong data");

	cr_assert(vmm_free(&right_space, right_id), "vmm_free failed for right user range");
	cr_assert(vmm_free(&left_space, left_id), "vmm_free failed for left user range");
	vmm_address_space_deinit(&right_space);
	vmm_address_space_deinit(&left_space);
}

Test(vmm, typed_address_transfer_helpers_allow_unaligned_cross_page_access) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct address_space    user_space = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 2,
			.align_pages = 1,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t  alloc_id      = VMM_ID_INVALID;
	void*     base          = NULL;
	uint64_t  u64_value     = 0x1122334455667788ull;
	uint64_t  read_u64      = 0u;
	uintptr_t uptr_value    = (uintptr_t)0xfeedc0de12345678ull;
	uintptr_t read_uintptr  = 0u;
	uint32_t  rejected_read = 0u;
	uintptr_t cross_page_addr;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(address_space_init(&user_space, MM_USER_VMM_BASE, 16u), "failed to initialize user address space");
	cr_assert(hal_paging_space_create(&user_space.hal_space), "failed to create user HAL address space");
	cr_assert(vmm_alloc(&user_space, &params, &alloc_id, &base), "failed to allocate typed helper user range");

	cross_page_addr = (uintptr_t)base + PMM_PAGE_SIZE - 3u;
	cr_assert_eq(address_space_write_u64(&user_space, cross_page_addr, u64_value),
	             ADDRESS_TRANSFER_OK,
	             "unaligned cross-page u64 write failed");
	cr_assert_eq(address_space_read_u64(&user_space, cross_page_addr, &read_u64),
	             ADDRESS_TRANSFER_OK,
	             "unaligned cross-page u64 read failed");
	cr_assert_eq(read_u64, u64_value, "u64 helper round-trip returned the wrong value");

	cr_assert_eq(address_space_write_uintptr(&user_space, (uintptr_t)base, uptr_value),
	             ADDRESS_TRANSFER_OK,
	             "uintptr write failed");
	cr_assert_eq(address_space_read_uintptr(&user_space, (uintptr_t)base, &read_uintptr),
	             ADDRESS_TRANSFER_OK,
	             "uintptr read failed");
	cr_assert_eq(read_uintptr, uptr_value, "uintptr helper round-trip returned the wrong value");

	cr_assert_eq(address_space_read_u32(&user_space, (uintptr_t)base + 2u * PMM_PAGE_SIZE, &rejected_read),
	             ADDRESS_TRANSFER_NOT_MAPPED,
	             "read outside allocation should fail");

	cr_assert(vmm_free(&user_space, alloc_id), "vmm_free failed for typed helper user range");
	vmm_address_space_deinit(&user_space);
}

Test(vmm, rejects_guard_pages_for_non_stack_allocations) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count  = 2,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind        = VMM_KIND_HEAP,
		.guard_pages = 1,
	};
	vmm_id_t alloc_id = VMM_ID_INVALID;
	void*    base     = NULL;

	init_test_vmm(arena, sizeof(arena));

	cr_assert(!vmm_alloc(address_space_kernel(), &params, &alloc_id, &base),
	          "vmm_alloc unexpectedly accepted guard pages for a heap allocation");
	cr_assert_eq(alloc_id, VMM_ID_INVALID, "failed guard-page validation changed the allocation id");
	cr_assert_null(base, "failed guard-page validation changed the allocation base");
	cr_assert_eq(vmm_count(address_space_kernel()), 0, "failed guard-page validation left tracked allocations behind");
}
