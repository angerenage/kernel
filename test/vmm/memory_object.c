#include <core/memory_object.h>
#include <core/memory_region.h>
#include <core/region_pager.h>

#include "test_support.h"

Test(vmm, anonymous_memory_object_is_process_independent_and_reference_counted) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory = NULL;
	size_t                 free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(2u, &memory), "anonymous object creation failed");
	cr_assert_not_null(memory, "anonymous object creation returned NULL");
	cr_assert_eq(memory_object_type(memory), MEMORY_OBJECT_ANONYMOUS, "anonymous object has the wrong type");
	cr_assert_eq(memory_object_page_count(memory), 2u, "anonymous object has the wrong logical size");
	cr_assert(memory_object_retain(memory), "anonymous object retain failed");
	memory_object_release(memory);
	memory_object_release(memory);
	cr_assert_eq(pmm_free_page_count(), free_before, "anonymous object metadata leaked after its last reference");
}

Test(vmm, region_retains_anonymous_object_until_region_destruction) {
	_Alignas(4096) uint8_t             arena[KiB(256)];
	struct memory_object*              memory = NULL;
	struct memory_region_create_result result;
	struct vmm_alloc_params            params = {
				   .page_count = 1u,
				   .prot       = VMM_PROT_READ | VMM_PROT_WRITE,
				   .kind       = VMM_KIND_GENERIC,
				   .map_flags  = VMM_MAP_LAZY,
    };
	uintptr_t backing_phys = 0u;
	size_t    free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(1u, &memory), "anonymous object creation failed");
	cr_assert(memory_region_create_with_object(address_space_kernel(), 0u, memory, 0u, &params, &result),
	          "region creation over anonymous object failed");
	memory_object_release(memory);

	cr_assert_eq(region_pager_map_all(address_space_kernel(), result.region),
	             REGION_PAGER_OK,
	             "region could not use its retained object reference");
	cr_assert(memory_object_page_phys(result.region->memory, 0u, &backing_phys),
	          "mapping did not materialize object backing");
	cr_assert_neq(backing_phys, 0u, "materialized anonymous backing was invalid");
	cr_assert(region_pager_unmap_all(address_space_kernel(), result.region, false), "region unmap failed");
	cr_assert(memory_region_destroy(address_space_kernel(), result.region), "region destruction failed");
	cr_assert_eq(
		pmm_free_page_count(), free_before, "last region reference leaked anonymous backing or object metadata");
}

Test(vmm, external_object_teardown_never_frees_caller_pages) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory        = NULL;
	uintptr_t              external_phys = 0u;
	size_t                 free_before;
	size_t                 free_with_external;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(pmm_alloc_pages(2u, &external_phys), "could not allocate caller-owned physical pages");
	free_with_external = pmm_free_page_count();
	cr_assert(memory_object_create_external(external_phys, 2u, &memory), "external object creation failed");
	cr_assert_eq(memory_object_type(memory), MEMORY_OBJECT_EXTERNAL_PHYSICAL, "external object has the wrong type");
	memory_object_release(memory);
	cr_assert_eq(pmm_free_page_count(), free_with_external, "external object teardown leaked metadata or freed data");
	cr_assert(pmm_free_pages(external_phys, 2u), "external object freed caller-owned pages");
	cr_assert_eq(pmm_free_page_count(), free_before, "external object test did not restore PMM state");
}

Test(vmm, region_object_offsets_are_bounds_checked_and_used_for_mapping) {
	_Alignas(4096) uint8_t             arena[KiB(256)];
	struct memory_object*              memory = NULL;
	struct memory_region_create_result result;
	struct vmm_alloc_params            params = {
				   .page_count = 2u,
				   .prot       = VMM_PROT_READ | VMM_PROT_WRITE,
				   .kind       = VMM_KIND_PHYSICAL,
    };
	uintptr_t       external_phys = 0u;
	uintptr_t       mapped_phys   = 0u;
	struct vmm_info info;
	size_t          free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(pmm_alloc_pages(3u, &external_phys), "could not allocate external physical range");
	cr_assert(memory_object_create_external(external_phys, 3u, &memory), "external object creation failed");
	cr_assert_not(memory_region_create_with_object(address_space_kernel(), 0u, memory, 2u, &params, &result),
	              "out-of-range object subrange was accepted");
	cr_assert_not(memory_region_create_with_object(address_space_kernel(), 0u, memory, SIZE_MAX, &params, &result),
	              "overflowing object offset was accepted");

	params.page_count = 1u;
	cr_assert(memory_region_create_with_object(address_space_kernel(), 0u, memory, 1u, &params, &result),
	          "valid object subrange was rejected");
	memory_object_release(memory);
	cr_assert_eq(
		region_pager_map_all(address_space_kernel(), result.region), REGION_PAGER_OK, "offset region mapping failed");
	cr_assert(hal_paging_query(hal_paging_kernel_space(), result.base, &mapped_phys, NULL),
	          "offset region did not create a PTE");
	cr_assert_eq(mapped_phys, external_phys + PMM_PAGE_SIZE, "region ignored its Memory Object page offset");
	memory_region_fill_info(result.region, &info);
	cr_assert_eq(info.first_phys, external_phys + PMM_PAGE_SIZE, "region query ignored its Memory Object page offset");
	cr_assert(region_pager_unmap_all(address_space_kernel(), result.region, true), "offset region unmap failed");
	cr_assert(memory_region_destroy(address_space_kernel(), result.region), "offset region destruction failed");
	cr_assert(pmm_free_pages(external_phys, 3u), "offset region freed externally owned pages");
	cr_assert_eq(pmm_free_page_count(), free_before, "offset region test leaked object or mapping metadata");
}
