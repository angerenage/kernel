#include <test_memory.h>

#include "../../core/memory/address_space_internal.h"
#include "test_support.h"

static _Alignas(TEST_MAPPING_GRANULE) uint8_t arena[KiB(192)];

static bool map_memory(struct address_space* space, struct memory* memory, uintptr_t address, size_t alignment,
                       size_t guard_before, size_t guard_after, memory_access_t access, struct mapping** out) {
	return address_space_map(space,
	                         &(const struct address_space_mapping_request){
								 .memory       = memory,
								 .address      = address,
								 .alignment    = alignment,
								 .guard_before = guard_before,
								 .guard_after  = guard_after,
								 .access       = access,
							 },
	                         out);
}

static void unmap_release(struct address_space* space, struct mapping* mapping) {
	cr_assert(address_space_unmap(space, mapping));
	mapping_release(mapping);
}

static uintptr_t test_reserved_start(const struct mapping* mapping) {
	return mapping->address - mapping->guard_before;
}

static uintptr_t test_reserved_end(const struct mapping* mapping) {
	return mapping->address + mapping->size + mapping->guard_after;
}

static int validate_subtree(struct mapping* node, struct mapping* parent, struct mapping** previous, size_t* count) {
	int left_height, right_height, expected_height;
	if (node == NULL) return 0;
	cr_assert_eq(node->parent, parent, "AVL parent link is inconsistent");
	left_height = validate_subtree(node->left, node, previous, count);
	if (*previous == NULL) cr_assert_eq(node->previous, NULL, "first tree node has a list predecessor");
	else {
		cr_assert_eq((*previous)->next, node, "tree inorder and forward list order differ");
		cr_assert_eq(node->previous, *previous, "tree inorder and backward list order differ");
		cr_assert_leq(test_reserved_end(*previous), test_reserved_start(node), "tree reservation order is invalid");
	}
	*previous = node;
	(*count)++;
	right_height    = validate_subtree(node->right, node, previous, count);
	expected_height = (left_height > right_height ? left_height : right_height) + 1;
	cr_assert_eq(node->height, expected_height, "stored AVL height is inconsistent");
	cr_assert_leq(left_height - right_height, 1, "AVL tree is left-heavy");
	cr_assert_leq(right_height - left_height, 1, "AVL tree is right-heavy");
	return expected_height;
}

static void validate_registry(struct address_space* space) {
	struct mapping* previous = NULL;
	struct mapping* first    = space->mapping_root;
	size_t          count    = 0u;
	while (first != NULL && first->left != NULL) first = first->left;
	(void)validate_subtree(space->mapping_root, NULL, &previous, &count);
	cr_assert_eq(space->mapping_first, first, "tree minimum and list head differ");
	cr_assert_eq(previous, space->mapping_last, "tree maximum and list tail differ");
	if (previous != NULL) cr_assert_eq(previous->next, NULL, "last tree node has a list successor");
	cr_assert_eq(count, space->mapping_count, "tree and registry counts differ");
}

Test(address_space, registry_survives_random_insert_and_remove_order) {
	static const uint8_t insert_order[] = {12, 3,  19, 0,  23, 7,  15, 1, 10, 21, 5,  17,
	                                       8,  14, 2,  20, 6,  11, 22, 4, 18, 9,  16, 13};
	static const uint8_t remove_order[] = {12, 0, 23, 7, 19, 3, 15, 21, 1, 10, 17, 5,
	                                       14, 8, 20, 2, 11, 6, 22, 4,  9, 18, 13, 16};
	struct memory*       root;
	struct memory*       views[sizeof(insert_order)]    = {0};
	struct mapping*      mappings[sizeof(insert_order)] = {0};
	uintptr_t            base                           = MM_KERNEL_ADDRESS_SPACE_BASE + 64u * TEST_MAPPING_GRANULE;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(sizeof(insert_order) * TEST_MAPPING_GRANULE, &root));
	for (size_t i = 0u; i < sizeof(insert_order); i++)
		cr_assert(memory_slice(root, i * TEST_MAPPING_GRANULE, TEST_MAPPING_GRANULE, &views[i]));
	for (size_t i = 0u; i < sizeof(insert_order); i++) {
		size_t index = insert_order[i];
		cr_assert(map_memory(address_space_kernel(),
		                     views[index],
		                     base + (index * 4u + 1u) * TEST_MAPPING_GRANULE,
		                     TEST_MAPPING_GRANULE,
		                     TEST_MAPPING_GRANULE,
		                     TEST_MAPPING_GRANULE,
		                     MEMORY_ACCESS_READ,
		                     &mappings[index]));
		cr_assert_eq(address_space_mapping_count(address_space_kernel()), i + 1u);
		validate_registry(address_space_kernel());
	}
	for (size_t i = 0u; i < sizeof(insert_order); i++) {
		cr_assert(address_space_contains_mapping(address_space_kernel(), mappings[i]));
		cr_assert(
			address_space_resolve_fault(address_space_kernel(), mapping_address(mappings[i]), MEMORY_ACCESS_READ));
		cr_assert_not(
			address_space_resolve_fault(address_space_kernel(), mapping_address(mappings[i]) - 1u, MEMORY_ACCESS_READ));
	}
	for (size_t i = 0u; i < sizeof(remove_order); i++) {
		size_t index = remove_order[i];
		unmap_release(address_space_kernel(), mappings[index]);
		mappings[index] = NULL;
		cr_assert_eq(address_space_mapping_count(address_space_kernel()), sizeof(remove_order) - i - 1u);
		validate_registry(address_space_kernel());
	}
	for (size_t i = 0u; i < sizeof(views) / sizeof(views[0]); i++) memory_release(views[i]);
	memory_release(root);
}

Test(address_space, placement_obeys_bounds_alignment_and_overflow) {
	struct memory*  memory;
	struct mapping* mapping;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE, &memory));
	cr_assert_not(map_memory(address_space_kernel(),
	                         memory,
	                         MM_KERNEL_ADDRESS_SPACE_BASE,
	                         TEST_MAPPING_GRANULE,
	                         TEST_MAPPING_GRANULE,
	                         0u,
	                         MEMORY_ACCESS_READ,
	                         &mapping));
	cr_assert_not(map_memory(address_space_kernel(),
	                         memory,
	                         UINTPTR_MAX & ~(TEST_MAPPING_GRANULE - 1u),
	                         TEST_MAPPING_GRANULE,
	                         0u,
	                         TEST_MAPPING_GRANULE,
	                         MEMORY_ACCESS_READ,
	                         &mapping));
	cr_assert_not(map_memory(address_space_kernel(),
	                         memory,
	                         MM_KERNEL_ADDRESS_SPACE_BASE + TEST_MAPPING_GRANULE,
	                         4u * TEST_MAPPING_GRANULE,
	                         0u,
	                         0u,
	                         MEMORY_ACCESS_READ,
	                         &mapping));
	cr_assert(map_memory(address_space_kernel(),
	                     memory,
	                     0u,
	                     4u * TEST_MAPPING_GRANULE,
	                     TEST_MAPPING_GRANULE,
	                     TEST_MAPPING_GRANULE,
	                     MEMORY_ACCESS_READ,
	                     &mapping));
	cr_assert_eq(mapping_address(mapping) & (4u * TEST_MAPPING_GRANULE - 1u), 0u);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, placement_alignment_guards_and_stable_identity) {
	struct memory * root, *first_memory, *second_memory, *third_memory;
	struct mapping *first, *second, *third;
	uintptr_t       fixed = MM_KERNEL_ADDRESS_SPACE_BASE + 16u * TEST_MAPPING_GRANULE;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(4u * TEST_MAPPING_GRANULE, &root));
	cr_assert(memory_slice(root, 0u, TEST_MAPPING_GRANULE, &first_memory));
	cr_assert(memory_slice(root, TEST_MAPPING_GRANULE, TEST_MAPPING_GRANULE, &second_memory));
	cr_assert(memory_slice(root, 2u * TEST_MAPPING_GRANULE, TEST_MAPPING_GRANULE, &third_memory));
	cr_assert(map_memory(address_space_kernel(),
	                     first_memory,
	                     fixed,
	                     TEST_MAPPING_GRANULE,
	                     2u * TEST_MAPPING_GRANULE,
	                     TEST_MAPPING_GRANULE,
	                     MEMORY_ACCESS_READ,
	                     &first));
	cr_assert_not(map_memory(address_space_kernel(),
	                         second_memory,
	                         fixed - TEST_MAPPING_GRANULE,
	                         TEST_MAPPING_GRANULE,
	                         0u,
	                         0u,
	                         MEMORY_ACCESS_READ,
	                         &second));
	cr_assert(map_memory(address_space_kernel(),
	                     second_memory,
	                     0u,
	                     8u * TEST_MAPPING_GRANULE,
	                     TEST_MAPPING_GRANULE,
	                     0u,
	                     MEMORY_ACCESS_READ,
	                     &second));
	cr_assert(map_memory(address_space_kernel(),
	                     third_memory,
	                     0u,
	                     TEST_MAPPING_GRANULE,
	                     0u,
	                     TEST_MAPPING_GRANULE,
	                     MEMORY_ACCESS_READ,
	                     &third));
	cr_assert_eq(mapping_address(second) & (8u * TEST_MAPPING_GRANULE - 1u), 0u);
	cr_assert_eq(address_space_mapping_count(address_space_kernel()), 3u);
	cr_assert(address_space_contains_mapping(address_space_kernel(), first));
	cr_assert_not(address_space_resolve_fault(address_space_kernel(), fixed - TEST_MAPPING_GRANULE, MEMORY_ACCESS_READ),
	              "guard resolved as Mapping contents");
	struct mapping* identity = second;
	unmap_release(address_space_kernel(), first);
	cr_assert_eq(second, identity, "removing another Mapping changed stable identity");
	unmap_release(address_space_kernel(), second);
	unmap_release(address_space_kernel(), third);
	memory_release(third_memory);
	memory_release(second_memory);
	memory_release(first_memory);
	memory_release(root);
}

Test(address_space, rejects_unaligned_memory_view_and_geometry) {
	struct memory * root, *slice;
	struct mapping* mapping;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE + 1u, &root));
	cr_assert(memory_slice(root, 1u, TEST_MAPPING_GRANULE, &slice));
	cr_assert_not(
		map_memory(address_space_kernel(), slice, 0u, TEST_MAPPING_GRANULE, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	cr_assert_not(
		map_memory(address_space_kernel(), root, 0u, TEST_MAPPING_GRANULE, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	memory_release(slice);
	memory_release(root);
}

Test(address_space, physical_zero_is_a_valid_backing) {
	struct memory*                zero;
	struct mapping*               mapping;
	struct hal_paging_translation translation;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address = 0u,
			.size             = TEST_MAPPING_GRANULE,
			.memory_type      = MEMORY_TYPE_NORMAL,
		},
		&zero));
	cr_assert(map_memory(address_space_kernel(), zero, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	cr_assert(address_space_resolve_fault(address_space_kernel(), mapping_address(mapping), MEMORY_ACCESS_READ));
	cr_assert(hal_paging_query(address_space_hal(address_space_kernel()), mapping_address(mapping), &translation));
	cr_assert_eq(translation.physical_address, 0u);
	unmap_release(address_space_kernel(), mapping);
	memory_release(zero);
}

Test(address_space, sparse_map_is_lazy_and_metadata_is_constant_size) {
	const size_t         sparse_size = (size_t)1u << 40u;
	struct address_space space       = {0};
	struct memory*       memory;
	struct mapping*      mapping;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(address_space_create_process(&space));
	space.end     = space.base + sparse_size;
	size_t before = pmm_free_size();
	cr_assert(memory_create_anonymous(sparse_size, &memory));
	cr_assert(map_memory(&space, memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	cr_assert_leq(
		before - pmm_free_size(), 3u * TEST_MAPPING_GRANULE, "a sparse Mapping allocated size-proportional metadata");
	cr_assert_not(hal_paging_query(address_space_hal(&space), mapping_address(mapping), NULL));
	unmap_release(&space, mapping);
	memory_release(memory);
	address_space_destroy(&space);
	cr_assert_eq(pmm_free_size(), before);
}

Test(address_space, faults_and_prefaults_expose_large_present_runs_to_hal) {
	const size_t    large_size = (size_t)1u << 21u;
	struct memory*  memory;
	struct mapping *fault_mapping, *prefault_mapping;
	uintptr_t       first_address  = MM_KERNEL_ADDRESS_SPACE_BASE + large_size;
	uintptr_t       second_address = first_address + 2u * large_size;

	init_test_address_space(arena, sizeof(arena));
	mock_paging_set_leaf_size_mask((1ull << 12) | (1ull << 21));
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address = 0u,
			.size             = large_size,
			.memory_type      = MEMORY_TYPE_NORMAL,
		},
		&memory));
	cr_assert(map_memory(
		address_space_kernel(), memory, first_address, large_size, 0u, 0u, MEMORY_ACCESS_READ, &fault_mapping));
	cr_assert(address_space_resolve_fault(
		address_space_kernel(), first_address + 17u * TEST_MAPPING_GRANULE, MEMORY_ACCESS_READ));
	cr_assert_eq(mock_paging_map_call_count(), 1u);
	cr_assert_eq(mock_paging_largest_map_size(), large_size);

	cr_assert(map_memory(
		address_space_kernel(), memory, second_address, large_size, 0u, 0u, MEMORY_ACCESS_READ, &prefault_mapping));
	cr_assert(address_space_prefault(address_space_kernel(), prefault_mapping, 0u, large_size));
	cr_assert_eq(mock_paging_map_call_count(), 2u);
	cr_assert_eq(mock_paging_largest_map_size(), large_size);
	unmap_release(address_space_kernel(), prefault_mapping);
	unmap_release(address_space_kernel(), fault_mapping);
	memory_release(memory);
}

Test(address_space, a_fault_maps_at_most_one_supported_leaf) {
	const size_t    leaf_size   = (size_t)1u << 21u;
	const size_t    memory_size = 2u * leaf_size;
	struct memory*  memory;
	struct mapping* mapping;
	uintptr_t       address = MM_KERNEL_ADDRESS_SPACE_BASE + 4u * leaf_size;

	init_test_address_space(arena, sizeof(arena));
	mock_paging_set_leaf_size_mask((1ull << 12) | (1ull << 21));
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address = 0u,
			.size             = memory_size,
			.memory_type      = MEMORY_TYPE_NORMAL,
		},
		&memory));
	cr_assert(map_memory(address_space_kernel(), memory, address, leaf_size, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	cr_assert(address_space_resolve_fault(
		address_space_kernel(), address + leaf_size + 17u * TEST_MAPPING_GRANULE, MEMORY_ACCESS_READ));
	cr_assert_eq(mock_paging_map_call_count(), 1u);
	cr_assert_eq(mock_paging_largest_map_size(), leaf_size);
	cr_assert_eq(mock_paging_mapping_count(), leaf_size / TEST_MAPPING_GRANULE);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, shared_memory_faults_reuse_backing) {
	struct address_space          a = {0}, b = {0};
	struct memory*                memory;
	struct mapping *              a_mapping, *b_mapping;
	struct hal_paging_translation a_translation, b_translation;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(address_space_create_process(&a));
	cr_assert(address_space_create_process(&b));
	cr_assert(memory_create_anonymous(2u * TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(&a, memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, &a_mapping));
	cr_assert(map_memory(&b, memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, &b_mapping));
	cr_assert(address_space_resolve_fault(&a, mapping_address(a_mapping), MEMORY_ACCESS_WRITE));
	cr_assert(address_space_resolve_fault(&b, mapping_address(b_mapping), MEMORY_ACCESS_READ));
	cr_assert(hal_paging_query(address_space_hal(&a), mapping_address(a_mapping), &a_translation));
	cr_assert(hal_paging_query(address_space_hal(&b), mapping_address(b_mapping), &b_translation));
	cr_assert_eq(a_translation.physical_address, b_translation.physical_address);
	unmap_release(&a, a_mapping);
	unmap_release(&b, b_mapping);
	address_space_destroy(&a);
	address_space_destroy(&b);
	memory_release(memory);
}

Test(address_space, protect_none_preserves_contents_and_future_access) {
	struct memory*                memory;
	struct mapping*               mapping;
	struct hal_paging_translation translation;
	uint8_t                       value = 0x5au, readback = 0u;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(2u * TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(address_space_kernel(), memory, 0u, 0u, 0u, 0u, 0u, &mapping));
	cr_assert_not(hal_paging_query(address_space_hal(address_space_kernel()), mapping_address(mapping), NULL));
	cr_assert(address_space_protect(address_space_kernel(), mapping, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE));
	cr_assert(address_space_resolve_fault(address_space_kernel(), mapping_address(mapping), MEMORY_ACCESS_WRITE));
	cr_assert(memory_write(memory, 0u, &value, sizeof(value)));
	cr_assert(address_space_protect(address_space_kernel(), mapping, MEMORY_ACCESS_READ));
	cr_assert(hal_paging_query(address_space_hal(address_space_kernel()), mapping_address(mapping), &translation));
	cr_assert_eq(translation.flags, HAL_PAGE_READ | HAL_PAGE_GLOBAL);
	cr_assert(address_space_protect(address_space_kernel(), mapping, 0u));
	cr_assert_not(hal_paging_query(address_space_hal(address_space_kernel()), mapping_address(mapping), NULL));
	cr_assert(address_space_protect(address_space_kernel(), mapping, MEMORY_ACCESS_WRITE));
	cr_assert(address_space_resolve_fault(
		address_space_kernel(), mapping_address(mapping) + TEST_MAPPING_GRANULE, MEMORY_ACCESS_WRITE));
	cr_assert(memory_read(memory, 0u, &readback, sizeof(readback)));
	cr_assert_eq(readback, value);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, failed_pte_install_keeps_materialized_memory) {
	struct memory*     memory;
	struct mapping*    mapping;
	struct memory_span span;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(address_space_kernel(), memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	mock_paging_fail_after(0u);
	cr_assert_not(address_space_resolve_fault(address_space_kernel(), mapping_address(mapping), MEMORY_ACCESS_READ));
	cr_assert(memory_query(memory, 0u, TEST_MAPPING_GRANULE, &span));
	cr_assert_eq(span.kind, MEMORY_SPAN_PRESENT);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, executable_projection_synchronizes_only_present_memory) {
	struct memory*     memory;
	struct mapping*    mapping;
	struct memory_span span;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(2u * TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(address_space_kernel(), memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	cr_assert(address_space_resolve_fault(address_space_kernel(), mapping_address(mapping), MEMORY_ACCESS_READ));
	cr_assert_eq(mock_cache_executable_sync_count(), 0u);
	cr_assert(address_space_protect(address_space_kernel(), mapping, MEMORY_ACCESS_READ | MEMORY_ACCESS_EXEC));
	cr_assert_eq(mock_cache_executable_sync_count(), 1u);
	cr_assert(memory_query(memory, TEST_MAPPING_GRANULE, TEST_MAPPING_GRANULE, &span));
	cr_assert_eq(span.kind, MEMORY_SPAN_HOLE);
	cr_assert(address_space_resolve_fault(
		address_space_kernel(), mapping_address(mapping) + TEST_MAPPING_GRANULE, MEMORY_ACCESS_EXEC));
	cr_assert_eq(mock_cache_executable_sync_count(), 2u);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, executable_prefault_synchronizes_present_run) {
	struct memory*  memory;
	struct mapping* mapping;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(2u * TEST_MAPPING_GRANULE, &memory));
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .offset             = 0u,
									 .size               = 2u * TEST_MAPPING_GRANULE,
									 .alignment          = TEST_MAPPING_GRANULE,
									 .require_contiguous = true,
								 }));
	cr_assert(
		map_memory(address_space_kernel(), memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ | MEMORY_ACCESS_EXEC, &mapping));
	cr_assert(address_space_prefault(address_space_kernel(), mapping, 0u, 2u * TEST_MAPPING_GRANULE));
	cr_assert_eq(mock_cache_executable_sync_count(), 1u);
	unmap_release(address_space_kernel(), mapping);
	memory_release(memory);
}

Test(address_space, inaccessible_external_memory_cannot_be_executable) {
	struct memory*  memory;
	struct mapping* mapping = NULL;

	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address        = (uintptr_t)arena + KiB(32),
			.size                    = TEST_MAPPING_GRANULE,
			.memory_type             = MEMORY_TYPE_NORMAL,
			.external_cpu_accessible = false,
		},
		&memory));
	cr_assert_not(
		map_memory(address_space_kernel(), memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ | MEMORY_ACCESS_EXEC, &mapping));
	cr_assert_null(mapping);
	memory_release(memory);
}

Test(address_space, detached_mapping_metadata_remains_readable) {
	struct memory*  memory;
	struct mapping* mapping;
	init_test_address_space(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(address_space_kernel(),
	                     memory,
	                     0u,
	                     0u,
	                     TEST_MAPPING_GRANULE,
	                     TEST_MAPPING_GRANULE,
	                     MEMORY_ACCESS_READ,
	                     &mapping));
	uintptr_t address = mapping_address(mapping);
	cr_assert(address_space_unmap(address_space_kernel(), mapping));
	cr_assert_not(address_space_contains_mapping(address_space_kernel(), mapping));
	cr_assert_eq(mapping_address(mapping), address);
	cr_assert_eq(mapping_size(mapping), TEST_MAPPING_GRANULE);
	cr_assert_eq(mapping_guard_before(mapping), TEST_MAPPING_GRANULE);
	cr_assert_eq(mapping_guard_after(mapping), TEST_MAPPING_GRANULE);
	cr_assert_eq(mapping_access(mapping), MEMORY_ACCESS_READ);
	mapping_release(mapping);
	memory_release(memory);
}

Test(address_space, process_destruction_detaches_but_preserves_mapping_metadata) {
	struct address_space space = {0};
	struct memory*       memory;
	struct mapping*      mapping;
	uintptr_t            address;
	size_t               free_before;

	init_test_address_space(arena, sizeof(arena));
	free_before = pmm_free_size();
	cr_assert(address_space_create_process(&space));
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE, &memory));
	cr_assert(map_memory(&space, memory, 0u, 0u, 0u, 0u, MEMORY_ACCESS_READ, &mapping));
	address = mapping_address(mapping);
	memory_release(memory);
	address_space_destroy(&space);
	cr_assert_not(address_space_is_initialized(&space));
	cr_assert_not(address_space_contains_mapping(&space, mapping));
	cr_assert_not(address_space_unmap(&space, mapping));
	cr_assert_eq(mapping_address(mapping), address);
	cr_assert_eq(mapping_size(mapping), TEST_MAPPING_GRANULE);
	cr_assert_eq(mapping_access(mapping), MEMORY_ACCESS_READ);
	mapping_release(mapping);
	cr_assert_eq(pmm_free_size(), free_before);
}
