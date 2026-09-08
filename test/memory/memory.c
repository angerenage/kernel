#include <core/memory.h>
#include <string.h>

#include "../memory_backing/test_support.h"

Test(memory, anonymous_size_is_exact_and_sparse_contents_are_zero) {
	struct memory*     memory;
	struct memory_span span;
	uint8_t            bytes[17];
	const uint8_t      value[] = {0x12u, 0x34u, 0x56u};
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	size_t before  = pmm_free_size();
	cr_assert(memory_create_anonymous(granule + 19u, &memory));
	cr_assert_eq(memory_size(memory), granule + 19u);
	cr_assert_eq(memory_type(memory), MEMORY_TYPE_NORMAL);
	cr_assert(memory_can_transfer(memory));
	cr_assert(memory_query(memory, granule, granule, &span));
	cr_assert_eq(span.kind, MEMORY_SPAN_HOLE);
	cr_assert_eq(span.size, 19u, "query escaped the logical memory size");
	memset(bytes, 0xff, sizeof(bytes));
	cr_assert(memory_read(memory, granule + 2u, bytes, sizeof(bytes)));
	cr_assert_arr_eq(bytes, (uint8_t[17]){0}, sizeof(bytes));
	cr_assert(memory_write(memory, granule + 16u, value, sizeof(value)));
	memset(bytes, 0, sizeof(bytes));
	cr_assert(memory_read(memory, granule + 16u, bytes, sizeof(value)));
	cr_assert_arr_eq(bytes, value, sizeof(value));
	cr_assert_not(memory_write(memory, granule + 17u, value, sizeof(value)));
	memory_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(memory, slices_share_contents_and_retain_their_immediate_parents) {
	struct memory *root, *slice, *nested, *sibling;
	uint8_t        value[]                 = {1u, 3u, 5u, 7u};
	uint8_t        readback[sizeof(value)] = {0};
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	size_t before  = pmm_free_size();
	cr_assert(memory_create_anonymous(2u * granule + 31u, &root));
	cr_assert(memory_slice(root, 11u, granule + 20u, &slice));
	cr_assert(memory_slice(slice, 13u, granule, &nested));
	cr_assert(memory_slice(root, 20u, granule, &sibling));
	cr_assert_eq(memory_size(nested), granule);
	cr_assert_eq(memory_type(nested), memory_type(root));
	cr_assert(memory_write(nested, 9u, value, sizeof(value)));
	cr_assert(memory_read(root, 33u, readback, sizeof(readback)));
	cr_assert_arr_eq(readback, value, sizeof(value));
	memset(readback, 0, sizeof(readback));
	cr_assert(memory_read(sibling, 13u, readback, sizeof(readback)));
	cr_assert_arr_eq(readback, value, sizeof(value));
	memory_release(root);
	memory_release(slice);
	memset(readback, 0, sizeof(readback));
	cr_assert(memory_read(nested, 9u, readback, sizeof(readback)));
	cr_assert_arr_eq(readback, value, sizeof(value));
	memory_release(sibling);
	memory_release(nested);
	cr_assert_eq(pmm_free_size(), before);
}

Test(memory, slice_materialization_translates_absolute_offsets) {
	struct memory *    root, *slice;
	struct memory_span root_span, slice_span;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_create_anonymous(4u * granule, &root));
	cr_assert(memory_slice(root, granule, 2u * granule, &slice));
	cr_assert(memory_materialize(slice,
	                             &(const struct memory_materialize_request){
									 .size               = granule,
									 .alignment          = granule,
									 .require_contiguous = true,
								 }));
	cr_assert(memory_query(slice, 0u, granule, &slice_span));
	cr_assert_eq(slice_span.kind, MEMORY_SPAN_PRESENT);
	cr_assert(memory_query(root, granule, granule, &root_span));
	cr_assert_eq(root_span.physical_address, slice_span.physical_address);
	cr_assert(memory_query(root, 0u, granule, &root_span));
	cr_assert_eq(root_span.kind, MEMORY_SPAN_HOLE);
	cr_assert_not(memory_materialize(slice, &(const struct memory_materialize_request){.offset = 1u, .size = granule}));
	memory_release(slice);
	memory_release(root);
}

Test(memory, range_alignment_uses_the_absolute_backing_offset) {
	struct memory *root, *unaligned, *aligned;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_create_anonymous(3u * granule, &root));
	cr_assert(memory_slice(root, 1u, granule, &unaligned));
	cr_assert(memory_slice(root, granule, granule, &aligned));
	cr_assert_not(memory_range_is_backing_aligned(unaligned, 0u, granule, granule));
	cr_assert(memory_range_is_backing_aligned(aligned, 0u, granule, granule));
	cr_assert_not(memory_range_is_backing_aligned(aligned, 0u, granule - 1u, granule));
	cr_assert_not(memory_range_is_backing_aligned(aligned, 0u, granule, 0u));
	cr_assert_not(memory_range_is_backing_aligned(aligned, 0u, granule, granule - 1u));
	memory_release(unaligned);
	memory_release(aligned);
	memory_release(root);
}

Test(memory, physical_memory_preserves_contents_and_device_rejects_transfers) {
	struct memory *normal, *device;
	uint8_t        readback[16];
	backing_test_pmm_init();
	size_t                         granule = backing_test_granule();
	static _Alignas(65536) uint8_t physical[65536];
	cr_assert_leq(granule, sizeof(physical));
	memset(physical, 0xa6, granule);
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address        = (uintptr_t)physical,
			.size                    = granule,
			.memory_type             = MEMORY_TYPE_NORMAL,
			.external_cpu_accessible = true,
		},
		&normal));
	cr_assert(memory_read(normal, 0u, readback, sizeof(readback)));
	for (size_t i = 0u; i < sizeof(readback); i++) cr_assert_eq(readback[i], 0xa6u);
	memory_release(normal);
	cr_assert_eq(physical[0], 0xa6u, "physical creation modified pre-existing contents");
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address        = (uintptr_t)physical,
			.size                    = granule,
			.memory_type             = MEMORY_TYPE_DEVICE,
			.external_cpu_accessible = true,
		},
		&device));
	cr_assert_not(memory_can_transfer(device));
	cr_assert_not(memory_read(device, 0u, readback, sizeof(readback)));
	cr_assert_not(memory_write(device, 0u, readback, sizeof(readback)));
	memory_release(device);
}

Test(memory, deeply_nested_slices_release_iteratively) {
	struct memory* current;
	struct memory* child;
	backing_test_pmm_init();
	size_t before = pmm_free_size();
	cr_assert(memory_create_anonymous(1u, &current));
	for (size_t depth = 0u; depth < 4096u; depth++) {
		cr_assert(memory_slice(current, 0u, 1u, &child));
		memory_release(current);
		current = child;
	}
	memory_release(current);
	cr_assert_eq(pmm_free_size(), before);
}
