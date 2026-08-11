#include "../vmm/test_support.h"

static void bind_exception_test_cpu(void) {
	hal_cpu_local_bind(NULL);
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
}

static void init_small_user_space(struct address_space* space) {
	struct hal_address_space hal_space;

	cr_assert_not_null(space);
	cr_assert(hal_paging_space_create(&hal_space), "hal_paging_space_create failed");
	cr_assert(address_space_init(space, 0x40000000u, 16u), "address_space_init failed");
	space->hal_space = hal_space;
}

Test(exception_fault, user_not_present_fault_materializes_only_the_current_user_space) {
	_Alignas(4096) uint8_t  arena[KiB(512)];
	struct address_space    user_space = {0};
	struct thread           current    = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 1u,
			.align_pages = 1u,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
			.kind        = VMM_KIND_HEAP,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t id   = VMM_ID_INVALID;
	void*    base = NULL;

	init_test_vmm(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);

	current.address_space         = &user_space;
	cpu_current()->current_thread = &current;

	cr_assert(vmm_alloc(&user_space, &params, &id, &base), "lazy user allocation failed");
	cr_assert_eq(mock_paging_mapping_count(), 0u);

	cr_assert(vmm_handle_current_page_fault((uintptr_t)base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_READ, true),
	          "valid userspace lazy fault was not resolved");
	cr_assert_eq(mock_paging_mapping_count(), 1u, "valid userspace fault did not materialize exactly one page");
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL, NULL),
	          "faulted page was not mapped in the current user address space");

	cpu_current()->current_thread = NULL;
	cr_assert(vmm_free(&user_space, id));
	vmm_address_space_deinit(&user_space);
	hal_cpu_local_bind(NULL);
}

Test(exception_fault, user_fault_cannot_materialize_a_lazy_kernel_mapping) {
	_Alignas(4096) uint8_t  arena[KiB(512)];
	struct address_space    user_space    = {0};
	struct thread           current       = {0};
	struct vmm_alloc_params kernel_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t kernel_id   = VMM_ID_INVALID;
	void*    kernel_base = NULL;
	size_t   mappings_before;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);

	current.address_space         = &user_space;
	cpu_current()->current_thread = &current;

	cr_assert(vmm_alloc(address_space_kernel(), &kernel_params, &kernel_id, &kernel_base),
	          "lazy kernel allocation failed");
	mappings_before = mock_paging_mapping_count();
	free_before     = pmm_free_page_count();

	cr_assert_not(
		vmm_handle_current_page_fault((uintptr_t)kernel_base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_READ, true),
		"userspace fault must not be satisfied from the kernel lazy address space");
	cr_assert_eq(mock_paging_mapping_count(), mappings_before, "userspace fault materialized a kernel-only mapping");
	cr_assert_eq(
		pmm_free_page_count(), free_before, "userspace fault allocated physical backing for a kernel-only region");

	cpu_current()->current_thread = NULL;
	cr_assert(vmm_free(address_space_kernel(), kernel_id));
	vmm_address_space_deinit(&user_space);
	hal_cpu_local_bind(NULL);
}

Test(exception_fault, forbidden_user_access_does_not_materialize_lazy_backing) {
	_Alignas(4096) uint8_t  arena[KiB(512)];
	struct address_space    user_space = {0};
	struct thread           current    = {0};
	struct vmm_alloc_params params     = {
			.page_count  = 1u,
			.align_pages = 1u,
			.prot        = VMM_PROT_READ | VMM_PROT_USER,
			.kind        = VMM_KIND_GENERIC,
			.map_flags   = VMM_MAP_LAZY,
    };
	vmm_id_t id   = VMM_ID_INVALID;
	void*    base = NULL;
	size_t   free_before;

	init_test_vmm(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);
	current.address_space         = &user_space;
	cpu_current()->current_thread = &current;

	cr_assert(vmm_alloc(&user_space, &params, &id, &base));
	free_before = pmm_free_page_count();
	cr_assert_not(vmm_handle_current_page_fault((uintptr_t)base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_WRITE, true));
	cr_assert_not(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL, NULL));
	cr_assert_eq(pmm_free_page_count(), free_before, "a rejected write fault must not allocate physical backing");

	cr_assert(vmm_handle_current_page_fault((uintptr_t)base, VMM_FAULT_NOT_PRESENT, VMM_FAULT_ACCESS_READ, true));
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL, NULL),
	          "an allowed read fault must still materialize userspace lazy backing");

	cpu_current()->current_thread = NULL;
	cr_assert(vmm_free(&user_space, id));
	vmm_address_space_deinit(&user_space);
	hal_cpu_local_bind(NULL);
}
