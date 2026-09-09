#include "../address_space/test_support.h"

static void bind_exception_test_cpu(void) {
	hal_cpu_local_bind(NULL);
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
}

static void init_small_user_space(struct address_space* space) {
	cr_assert_not_null(space);
	cr_assert(address_space_create_process(space), "address_space_create_process failed");
}

Test(exception_fault, user_not_present_fault_materializes_only_the_current_user_space) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space = {0};
	struct thread          current    = {0};
	struct mapping*        mapping    = NULL;
	void*                  base       = NULL;

	init_test_address_space(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);

	current.address_space = &user_space;
	cpu_current_thread_store(cpu_current(), &current);

	cr_assert(test_address_space_map(&user_space,
	                                 TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 0u,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &mapping,
	                                 &base));
	cr_assert_eq(mock_paging_mapping_count(), 0u);

	cr_assert(
		address_space_handle_current_fault((uintptr_t)base, ADDRESS_SPACE_FAULT_NOT_PRESENT, MEMORY_ACCESS_READ, true),
		"valid userspace lazy fault was not resolved");
	cr_assert_eq(mock_paging_mapping_count(), 1u, "valid userspace fault did not materialize exactly one page");
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL),
	          "faulted page was not mapped in the current user address space");

	cpu_current_thread_store(cpu_current(), NULL);
	cr_assert(address_space_unmap(&user_space, mapping));
	mapping_release(mapping);
	address_space_destroy(&user_space);
	hal_cpu_local_bind(NULL);
}

Test(exception_fault, user_fault_cannot_materialize_a_lazy_kernel_mapping) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space     = {0};
	struct thread          current        = {0};
	struct mapping*        kernel_mapping = NULL;
	void*                  kernel_base    = NULL;
	size_t                 mappings_before;
	size_t                 free_before;

	init_test_address_space(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);

	current.address_space = &user_space;
	cpu_current_thread_store(cpu_current(), &current);

	cr_assert(test_address_space_map(address_space_kernel(),
	                                 TEST_MAPPING_GRANULE,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE | 0u,
	                                 0u,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 &kernel_mapping,
	                                 &kernel_base),
	          "lazy kernel allocation failed");
	mappings_before = mock_paging_mapping_count();
	free_before     = pmm_free_size();

	cr_assert_not(address_space_handle_current_fault(
					  (uintptr_t)kernel_base, ADDRESS_SPACE_FAULT_NOT_PRESENT, MEMORY_ACCESS_READ, true),
	              "userspace fault must not be satisfied from the kernel lazy address space");
	cr_assert_eq(mock_paging_mapping_count(), mappings_before, "userspace fault materialized a kernel-only mapping");
	cr_assert_eq(pmm_free_size(), free_before, "userspace fault allocated physical backing for a kernel-only region");

	cpu_current_thread_store(cpu_current(), NULL);
	cr_assert(address_space_unmap(address_space_kernel(), kernel_mapping));
	mapping_release(kernel_mapping);
	address_space_destroy(&user_space);
	hal_cpu_local_bind(NULL);
}

Test(exception_fault, forbidden_user_access_does_not_materialize_lazy_backing) {
	_Alignas(4096) uint8_t arena[KiB(512)];
	struct address_space   user_space = {0};
	struct thread          current    = {0};
	struct mapping*        mapping    = NULL;
	void*                  base       = NULL;
	size_t                 free_before;

	init_test_address_space(arena, sizeof(arena));
	bind_exception_test_cpu();
	init_small_user_space(&user_space);
	current.address_space = &user_space;
	cpu_current_thread_store(cpu_current(), &current);

	cr_assert(test_address_space_map(
		&user_space, TEST_MAPPING_GRANULE, MEMORY_ACCESS_READ, 0u, TEST_MAPPING_GRANULE, 0u, &mapping, &base));
	free_before = pmm_free_size();
	cr_assert_not(address_space_handle_current_fault(
		(uintptr_t)base, ADDRESS_SPACE_FAULT_NOT_PRESENT, MEMORY_ACCESS_WRITE, true));
	cr_assert_not(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL));
	cr_assert_eq(pmm_free_size(), free_before, "a rejected write fault must not allocate physical backing");

	cr_assert(
		address_space_handle_current_fault((uintptr_t)base, ADDRESS_SPACE_FAULT_NOT_PRESENT, MEMORY_ACCESS_READ, true));
	cr_assert(hal_paging_query(address_space_hal(&user_space), (uintptr_t)base, NULL),
	          "an allowed read fault must still materialize userspace lazy backing");

	cpu_current_thread_store(cpu_current(), NULL);
	cr_assert(address_space_unmap(&user_space, mapping));
	mapping_release(mapping);
	address_space_destroy(&user_space);
	hal_cpu_local_bind(NULL);
}
