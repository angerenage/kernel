#include "test_support.h"

Test(vmm, failed_reinitialization_disables_operations_until_init_succeeds_again) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 1u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
		.map_flags  = VMM_MAP_LAZY,
	};
	vmm_id_t id   = (vmm_id_t)0x1234u;
	void*    base = (void*)(uintptr_t)0x1234u;

	init_test_vmm(arena, sizeof(arena));

	mock_paging_fail_init_once();
	cr_assert_not(vmm_init(), "injected paging initialization failure was not surfaced");

	cr_assert_not(vmm_alloc(address_space_kernel(), &params, &id, &base),
	              "VMM operations must remain disabled after vmm_init reports failure");
	cr_assert_eq(id, VMM_ID_INVALID, "disabled allocation must clear the output id");
	cr_assert_null(base, "disabled allocation must clear the output base");

	cr_assert(vmm_init(), "VMM must be able to initialize again after a transient backend failure");
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base),
	          "VMM operations did not recover after successful reinitialization");
	cr_assert(vmm_free(address_space_kernel(), id), "post-recovery allocation cleanup failed");
}

Test(vmm, repeated_successful_initialization_starts_with_empty_tracking) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 1u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
		.map_flags  = VMM_MAP_LAZY,
	};
	vmm_id_t id   = VMM_ID_INVALID;
	void*    base = NULL;

	init_test_vmm(arena, sizeof(arena));
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "pre-reinit allocation failed");
	cr_assert_eq(vmm_count(address_space_kernel()), 1u, "pre-reinit allocation was not tracked");

	cr_assert(vmm_init(), "second vmm_init failed");
	cr_assert_eq(
		vmm_count(address_space_kernel()), 0u, "successful reinitialization must reset tracked allocation metadata");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "successful reinitialization must start without stale mappings");
}
