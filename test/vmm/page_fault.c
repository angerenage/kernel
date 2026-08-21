#include "test_support.h"

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
	size_t          free_before_fault;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy vmm_alloc failed");
	free_before_fault = pmm_free_page_count();
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy reserved allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "fault resolution mapped more than the faulting page");
	cr_assert_lt(vmm_test_pages_consumed_since(free_before_fault),
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
	cr_assert_geq(vmm_test_pages_consumed_since(free_before_fault),
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
	size_t          free_before_fault;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();

	cr_assert(vmm_alloc(address_space_kernel(), &params, &alloc_id, &base), "lazy stack vmm_alloc failed");
	free_before_fault = pmm_free_page_count();
	cr_assert(!vmm_query(address_space_kernel(), (uint8_t*)base - PMM_PAGE_SIZE, &info),
	          "stack guard page was exposed as a normal allocation");
	cr_assert(!vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + 1u * (uintptr_t)PMM_PAGE_SIZE),
	          "stack fault resolution unexpectedly skipped the top page");
	cr_assert(vmm_resolve_page_fault(address_space_kernel(), (uintptr_t)base + 2u * (uintptr_t)PMM_PAGE_SIZE),
	          "vmm_resolve_page_fault failed for lazy stack allocation");
	cr_assert_eq(mock_paging_mapping_count(), 1, "stack fault resolution mapped more than the faulting page");
	cr_assert_lt(vmm_test_pages_consumed_since(free_before_fault),
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

Test(vmm, current_page_fault_handling_uses_thread_space_then_kernel_space) {
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

	cr_assert(vmm_handle_current_page_fault((uintptr_t)user_base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_READ, false),
	          "current fault handling did not handle the user address space");
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)user_base, &phys, &flags),
	          "user fault did not map into the current thread address space");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_USER), "user fault used incorrect mapping flags");

	cr_assert(
		vmm_handle_current_page_fault((uintptr_t)kernel_base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_READ, false),
		"current fault handling did not fall back to the kernel address space");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), (uintptr_t)kernel_base, &phys, &flags),
	          "kernel fallback fault did not map into the kernel address space");
	cr_assert_eq(flags, (uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_GLOBAL), "kernel fault used incorrect mapping flags");

	cpu_current()->current_thread = NULL;
	cr_assert(vmm_free(address_space_kernel(), kernel_id), "vmm_free failed for kernel allocation");
	cr_assert(vmm_free(&user_space, user_id), "vmm_free failed for user allocation");
	vmm_address_space_deinit(&user_space);
	hal_cpu_local_bind(NULL);
}
