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
	struct memory_object*  memory              = NULL;
	uintptr_t              external_phys       = 0u;
	uintptr_t              resolved_phys       = 0u;
	const size_t           external_page_count = (size_t)1u << 20u;
	size_t                 free_before;
	size_t                 free_with_external;
	bool                   allocated = true;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(pmm_alloc_pages(2u, &external_phys), "could not allocate caller-owned physical pages");
	free_with_external = pmm_free_page_count();
	cr_assert(memory_object_create_external(external_phys, external_page_count, &memory),
	          "large external object creation failed");
	cr_assert_eq(memory_object_type(memory), MEMORY_OBJECT_EXTERNAL_PHYSICAL, "external object has the wrong type");
	cr_assert_eq(pmm_free_page_count(), free_with_external, "external object allocated per-page backing metadata");
	cr_assert(memory_object_ensure_page(memory, external_page_count - 1u, &resolved_phys, &allocated),
	          "external object could not resolve a high page offset");
	cr_assert_eq(resolved_phys,
	             external_phys + (external_page_count - 1u) * (uintptr_t)PMM_PAGE_SIZE,
	             "external object resolved the wrong physical offset");
	cr_assert_not(allocated, "external object reported caller-owned backing as newly allocated");
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

Test(vmm, huge_anonymous_object_is_empty_until_one_high_page_is_materialized) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory = NULL;
	struct backing_store*  backing;
	uintptr_t              phys = 0u;
	size_t                 free_before;
	bool                   allocated = false;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(SIZE_MAX, &memory), "huge anonymous object creation failed");
	backing = &memory->backing.anonymous;
	cr_assert_eq(pmm_free_page_count(), free_before, "empty huge object allocated backing metadata or data");
	cr_assert_eq(backing->root_phys, 0u, "empty huge object allocated a radix root");
	cr_assert_eq(backing->resident_page_count, 0u, "empty huge object reported resident data");
	cr_assert_eq(backing->metadata_page_count, 0u, "empty huge object reported radix metadata");
	cr_assert_leq(backing->tree_depth, BACKING_STORE_MAX_DEPTH, "huge object exceeded the bounded radix depth");

	cr_assert(memory_object_ensure_page(memory, SIZE_MAX - 1u, &phys, &allocated),
	          "high logical page materialization failed");
	cr_assert(allocated, "high logical page was not reported as newly allocated");
	cr_assert_neq(phys, 0u, "high logical page received an invalid physical address");
	cr_assert_eq(backing->resident_page_count, 1u, "one materialization did not create exactly one resident page");
	cr_assert_eq(backing->metadata_page_count,
	             backing->tree_depth,
	             "first materialization did not allocate exactly one radix path");
	cr_assert_eq(free_before - pmm_free_page_count(),
	             backing->tree_depth + 1u,
	             "one high page consumed metadata proportional to its index");

	cr_assert(memory_object_release_page(memory, SIZE_MAX - 1u), "high logical page release failed");
	cr_assert_eq(backing->root_phys, 0u, "last resident page release did not prune the radix root");
	cr_assert_eq(backing->resident_page_count, 0u, "last resident page release retained resident accounting");
	cr_assert_eq(backing->metadata_page_count, 0u, "last resident page release retained radix metadata");
	cr_assert_eq(pmm_free_page_count(), free_before, "last resident page release leaked data or radix metadata");
	memory_object_release(memory);
}

Test(vmm, sparse_pages_share_nearby_paths_and_keep_distant_contents_independent) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory = NULL;
	struct backing_store*  backing;
	uintptr_t              near_first  = 0u;
	uintptr_t              near_second = 0u;
	uintptr_t              distant     = 0u;
	uintptr_t              queried     = 0u;
	size_t                 metadata_after_first;
	size_t                 free_before;
	bool                   allocated;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(SIZE_MAX, &memory), "sparse object creation failed");
	backing = &memory->backing.anonymous;
	cr_assert(memory_object_ensure_page(memory, 1u, &near_first, &allocated), "first nearby page failed");
	metadata_after_first = backing->metadata_page_count;
	cr_assert(memory_object_ensure_page(memory, 2u, &near_second, &allocated), "second nearby page failed");
	cr_assert_eq(backing->metadata_page_count, metadata_after_first, "nearby page did not reuse its radix path");
	cr_assert(memory_object_ensure_page(memory, SIZE_MAX - 2u, &distant, &allocated), "distant page failed");
	cr_assert_leq(backing->metadata_page_count,
	              backing->tree_depth * 2u,
	              "distant page allocated metadata proportional to its index");
	cr_assert_neq(near_first, near_second, "nearby logical pages shared a physical data page");
	cr_assert_neq(near_first, distant, "distant logical pages shared a physical data page");
	*(uint64_t*)(near_first + boot_info.direct_map_offset) = UINT64_C(0x1111222233334444);
	*(uint64_t*)(distant + boot_info.direct_map_offset)    = UINT64_C(0xaaaabbbbccccdddd);
	cr_assert(memory_object_page_phys(memory, 1u, &queried), "first nearby page lookup failed");
	cr_assert_eq(*(uint64_t*)(queried + boot_info.direct_map_offset),
	             UINT64_C(0x1111222233334444),
	             "first nearby page lost its contents");
	cr_assert(memory_object_page_phys(memory, SIZE_MAX - 2u, &queried), "distant page lookup failed");
	cr_assert_eq(*(uint64_t*)(queried + boot_info.direct_map_offset),
	             UINT64_C(0xaaaabbbbccccdddd),
	             "distant page lost its independent contents");

	cr_assert(memory_object_release_page(memory, 2u), "second nearby page release failed");
	cr_assert(memory_object_page_phys(memory, 1u, &queried), "releasing a nearby page removed its sibling");
	cr_assert(memory_object_page_phys(memory, SIZE_MAX - 2u, &queried),
	          "releasing a nearby page removed distant backing");
	cr_assert(memory_object_release_page(memory, 1u), "first nearby page release failed");
	cr_assert(memory_object_page_phys(memory, SIZE_MAX - 2u, &queried),
	          "pruning the nearby branch removed distant backing");
	cr_assert(memory_object_release_page(memory, SIZE_MAX - 2u), "distant page release failed");
	cr_assert_eq(backing->root_phys, 0u, "releasing every page did not restore the empty tree");
	cr_assert_eq(pmm_free_page_count(), free_before, "sparse page release leaked data or radix metadata");
	memory_object_release(memory);
}

Test(vmm, destroying_huge_sparse_object_walks_only_allocated_backing) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory = NULL;
	uintptr_t              phys;
	bool                   allocated;
	size_t                 free_before;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(SIZE_MAX, &memory), "huge sparse object creation failed");
	cr_assert(memory_object_ensure_page(memory, 0u, &phys, &allocated), "low sparse page failed");
	cr_assert(memory_object_ensure_page(memory, SIZE_MAX - 1u, &phys, &allocated), "high sparse page failed");
	memory_object_release(memory);
	cr_assert_eq(pmm_free_page_count(), free_before, "huge sparse object destruction leaked allocated backing");
}

Test(vmm, failed_sparse_path_construction_leaks_nothing_and_publishes_nothing) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct memory_object*  memory = NULL;
	uintptr_t              held_pages[128];
	uintptr_t              phys       = UINTPTR_MAX;
	size_t                 held_count = 0u;
	size_t                 depth;
	size_t                 free_before;
	size_t                 failure_budget;
	bool                   allocated = true;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(memory_object_create_anonymous(SIZE_MAX, &memory), "failure-test object creation failed");
	depth = memory->backing.anonymous.tree_depth;
	while (held_count < sizeof(held_pages) / sizeof(held_pages[0]) && pmm_alloc_pages(1u, &held_pages[held_count]))
		held_count++;
	cr_assert_lt(held_count, sizeof(held_pages) / sizeof(held_pages[0]), "failure test could not exhaust the PMM");
	cr_assert_geq(held_count, depth, "failure test exhausted too few PMM pages");
	for (size_t page = 0u; page < depth; page++) {
		held_count--;
		cr_assert(pmm_free_pages(held_pages[held_count], 1u), "could not establish sparse failure budget");
	}
	failure_budget = pmm_free_page_count();
	cr_assert_eq(failure_budget, depth, "unexpected sparse failure allocation budget");
	cr_assert_not(memory_object_ensure_page(memory, SIZE_MAX - 1u, &phys, &allocated),
	              "sparse path construction succeeded without a data-page budget");
	cr_assert_eq(phys, 0u, "failed sparse path construction retained a physical result");
	cr_assert_not(allocated, "failed sparse path construction reported an allocated page");
	cr_assert_eq(memory->backing.anonymous.root_phys, 0u, "failed sparse path construction published a root");
	cr_assert_eq(memory->backing.anonymous.metadata_page_count,
	             0u,
	             "failed sparse path construction retained metadata accounting");
	cr_assert_eq(memory->backing.anonymous.resident_page_count,
	             0u,
	             "failed sparse path construction retained resident accounting");
	cr_assert_eq(pmm_free_page_count(), failure_budget, "failed sparse path construction leaked PMM pages");
	while (held_count > 0u) {
		held_count--;
		cr_assert(pmm_free_pages(held_pages[held_count], 1u), "failure test could not restore a held PMM page");
	}
	memory_object_release(memory);
	cr_assert_eq(pmm_free_page_count(), free_before, "sparse failure test did not restore PMM accounting");
}

Test(vmm, huge_lazy_region_fault_materializes_only_one_sparse_page) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct vmm_alloc_params params = {
		.page_count = 131072u,
		.prot       = VMM_PROT_READ | VMM_PROT_WRITE,
		.kind       = VMM_KIND_GENERIC,
		.map_flags  = VMM_MAP_LAZY,
	};
	struct memory_region* region;
	vmm_id_t              id   = VMM_ID_INVALID;
	void*                 base = NULL;
	size_t                free_before;
	size_t                free_before_fault;

	init_test_vmm(arena, sizeof(arena));
	free_before = pmm_free_page_count();
	cr_assert(vmm_alloc(address_space_kernel(), &params, &id, &base), "huge lazy VMM allocation failed");
	region = memory_region_find_by_id(address_space_kernel(), id);
	cr_assert_not_null(region, "huge lazy VMM region was not tracked");
	cr_assert_eq(region->memory->backing.anonymous.root_phys, 0u, "huge lazy VMM allocation created backing");
	free_before_fault = pmm_free_page_count();
	cr_assert(vmm_resolve_page_fault(address_space_kernel(),
	                                 (uintptr_t)base + (params.page_count - 1u) * (uintptr_t)PMM_PAGE_SIZE),
	          "high fault in huge lazy VMM region failed");
	cr_assert_eq(region->memory->backing.anonymous.resident_page_count,
	             1u,
	             "huge lazy fault materialized more than one data page");
	cr_assert_eq(region->memory->backing.anonymous.metadata_page_count,
	             region->memory->backing.anonymous.tree_depth,
	             "huge lazy fault did not create one bounded radix path");
	cr_assert_leq(free_before_fault - pmm_free_page_count(),
	              region->memory->backing.anonymous.tree_depth + 2u,
	              "huge lazy fault consumed metadata proportional to the region size");
	cr_assert(vmm_free(address_space_kernel(), id), "huge lazy VMM region cleanup failed");
	cr_assert_eq(pmm_free_page_count(), free_before, "huge lazy VMM region leaked sparse backing");
}
