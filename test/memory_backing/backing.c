#include <pthread.h>
#include <string.h>

#include "../../core/memory/memory_backing_tree.h"
#include "test_support.h"

static bool create_physical(uintptr_t address, size_t size, bool external_cpu_accessible,
                            struct memory_backing** out_backing) {
	return memory_backing_create_physical(
		&(const struct memory_backing_physical_request){
			.physical_address        = address,
			.size                    = size,
			.external_cpu_accessible = external_cpu_accessible,
		},
		out_backing);
}

Test(memory_backing, sparse_query_zero_read_and_partial_write) {
	struct memory_backing*     backing;
	struct memory_backing_span span;
	uint8_t                    bytes[32];
	const uint8_t              value[] = {0x31u, 0x72u, 0xa5u};
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	size_t before  = pmm_free_size();
	cr_assert(memory_backing_create_anonymous(8u * granule, &backing));
	cr_assert(memory_backing_query(backing, 0u, 8u * granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_HOLE);
	cr_assert_eq(span.size, 8u * granule);
	cr_assert(memory_backing_read(backing, 3u * granule + 7u, bytes, sizeof(bytes)));
	cr_assert_arr_eq(bytes, (uint8_t[32]){0}, sizeof(bytes));
	cr_assert(memory_backing_query(backing, 0u, 8u * granule, &span));
	cr_assert_eq(span.size, 8u * granule);
	cr_assert(memory_backing_write(backing, 2u * granule + 11u, value, sizeof(value)));
	memset(bytes, 0xff, sizeof(bytes));
	cr_assert(memory_backing_read(backing, 2u * granule, bytes, sizeof(bytes)));
	for (size_t i = 0u; i < 11u; i++) cr_assert_eq(bytes[i], 0u);
	cr_assert_arr_eq(bytes + 11u, value, sizeof(value));
	for (size_t i = 14u; i < sizeof(bytes); i++) cr_assert_eq(bytes[i], 0u);
	cr_assert(memory_backing_query(backing, 2u * granule, granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_PRESENT);
	cr_assert_eq(span.size, granule);
	memory_backing_release(backing);
	cr_assert_eq(pmm_free_size(), before);
}

Test(memory_backing, contiguous_runs_constraints_and_atomic_failure) {
	struct memory_backing*     backing;
	struct memory_backing_span span;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_backing_create_anonymous(16u * granule, &backing));
	uintptr_t minimum = (uintptr_t)backing_test_arena + 512u * 1024u;
	uintptr_t maximum = minimum + 128u * 1024u;
	cr_assert(memory_backing_materialize(backing,
	                                     &(const struct memory_backing_materialize_request){
											 .offset             = 2u * granule,
											 .size               = 4u * granule,
											 .alignment          = 65536u,
											 .minimum_address    = minimum,
											 .maximum_address    = maximum,
											 .require_contiguous = true,
										 }));
	cr_assert(memory_backing_query(backing, 2u * granule, 4u * granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_PRESENT);
	cr_assert_eq(span.size, 4u * granule);
	cr_assert_geq(span.physical_address, minimum);
	cr_assert_leq(span.physical_address + span.size, maximum);
	cr_assert_eq(span.physical_address & 65535u, 0u);
	cr_assert(memory_backing_materialize(backing,
	                                     &(const struct memory_backing_materialize_request){
											 .offset             = 3u * granule,
											 .size               = granule,
											 .alignment          = 2u * 65536u,
											 .require_contiguous = true,
										 }));
	size_t free_before_failure = pmm_free_size();
	cr_assert_not(memory_backing_materialize(backing,
	                                         &(const struct memory_backing_materialize_request){
												 .offset          = 8u * granule,
												 .size            = 2u * granule,
												 .minimum_address = maximum,
												 .maximum_address = maximum + granule,
											 }));
	cr_assert_eq(pmm_free_size(), free_before_failure);
	cr_assert(memory_backing_query(backing, 8u * granule, 2u * granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_HOLE);
	memory_backing_release(backing);
}

Test(memory_backing, repeated_and_neighbor_materialization_coalesces) {
	struct memory_backing*     backing;
	struct memory_backing_span first, combined;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_backing_create_anonymous(4u * granule, &backing));
	cr_assert(memory_backing_materialize(
		backing, &(const struct memory_backing_materialize_request){.offset = 0u, .size = granule}));
	cr_assert(memory_backing_query(backing, 0u, granule, &first));
	size_t free_after_first = pmm_free_size();
	cr_assert(memory_backing_materialize(
		backing, &(const struct memory_backing_materialize_request){.offset = 0u, .size = granule}));
	cr_assert_eq(pmm_free_size(), free_after_first);
	cr_assert(memory_backing_materialize(backing,
	                                     &(const struct memory_backing_materialize_request){
											 .offset = 0u, .size = 2u * granule, .require_contiguous = true}));
	cr_assert(memory_backing_query(backing, 0u, 2u * granule, &combined));
	cr_assert_eq(combined.physical_address, first.physical_address);
	cr_assert_eq(combined.size, 2u * granule);
	cr_assert(memory_backing_materialize(
		backing, &(const struct memory_backing_materialize_request){.offset = 3u * granule, .size = granule}));
	cr_assert(memory_backing_query(backing, 2u * granule, granule, &combined));
	cr_assert_eq(combined.kind, MEMORY_BACKING_SPAN_HOLE);
	memory_backing_release(backing);
}

Test(memory_backing, logically_adjacent_noncontiguous_extents_do_not_merge) {
	struct memory_backing*     backing;
	struct memory_backing_span first;
	struct pmm_extent          blocker;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_backing_create_anonymous(2u * granule, &backing));
	cr_assert(memory_backing_materialize(
		backing, &(const struct memory_backing_materialize_request){.offset = 0u, .size = granule}));
	cr_assert(pmm_alloc(&(const struct pmm_alloc_request){.size = granule}, &blocker));
	cr_assert(memory_backing_materialize(
		backing, &(const struct memory_backing_materialize_request){.offset = granule, .size = granule}));
	cr_assert(memory_backing_query(backing, 0u, 2u * granule, &first));
	cr_assert_eq(first.kind, MEMORY_BACKING_SPAN_PRESENT);
	cr_assert_eq(first.size, granule);
	memory_backing_release(backing);
	cr_assert(pmm_free(blocker));
}

Test(memory_backing, physical_claim_ownership_and_external_overlap) {
	struct memory_backing *    first, *second, *adjacent;
	struct memory_backing_span span;
	backing_test_pmm_init();
	size_t    granule  = backing_test_granule();
	size_t    before   = pmm_free_size();
	uintptr_t external = 0x100000u;
	cr_assert_not(create_physical(UINTPTR_MAX & ~(uintptr_t)(granule - 1u), granule, false, &second));
	cr_assert(create_physical(external, 2u * granule, false, &first));
	cr_assert_not(memory_backing_cpu_accessible(first));
	cr_assert_not(memory_backing_read(first, 0u, &span, 1u));
	cr_assert_not(create_physical(external + granule, granule, true, &second));
	cr_assert(create_physical(external + 2u * granule, granule, true, &adjacent));
	cr_assert(memory_backing_cpu_accessible(adjacent));
	cr_assert(memory_backing_query(first, 0u, 2u * granule, &span));
	cr_assert_eq(span.physical_address, external);
	memory_backing_release(first);
	cr_assert(create_physical(external, granule, false, &second));
	memory_backing_release(second);
	memory_backing_release(adjacent);
	cr_assert_eq(pmm_free_size(), before);
}

Test(memory_backing, managed_claim_and_physical_zero_are_valid) {
	struct memory_backing*     backing;
	struct memory_backing_span span;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	size_t before  = pmm_free_size();
	cr_assert(create_physical(0u, granule, false, &backing));
	cr_assert_not(memory_backing_cpu_accessible(backing));
	cr_assert(memory_backing_query(backing, 0u, granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_PRESENT);
	cr_assert_eq(span.physical_address, 0u);
	cr_assert(memory_backing_retain(backing));
	memory_backing_release(backing);
	memory_backing_release(backing);
	cr_assert_eq(pmm_free_size(), before);
}

Test(memory_backing, managed_claim_is_released_and_partial_intersection_is_rejected) {
	struct memory_backing*     backing;
	struct memory_backing_span span;
	struct pmm_extent          allocation;
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	size_t before  = pmm_free_size();
	cr_assert(pmm_alloc(
		&(const struct pmm_alloc_request){
			.size            = 2u * granule,
			.minimum_address = (uintptr_t)backing_test_arena + 512u * 1024u,
		},
		&allocation));
	cr_assert(pmm_free(allocation));
	cr_assert(create_physical(allocation.address, allocation.size, false, &backing));
	cr_assert(memory_backing_cpu_accessible(backing));
	cr_assert(memory_backing_query(backing, granule, granule, &span));
	cr_assert_eq(span.physical_address, allocation.address + granule);
	memory_backing_release(backing);
	cr_assert_eq(pmm_free_size(), before);
	cr_assert_not(create_physical(
		(uintptr_t)backing_test_arena + BACKING_TEST_ARENA_SIZE - granule, 2u * granule, false, &backing));
	cr_assert_eq(pmm_free_size(), before);
}

struct materialize_thread_context {
	struct memory_backing* backing;
	size_t                 granule;
	bool                   result;
};

static void* materialize_worker(void* argument) {
	struct materialize_thread_context* context = argument;
	context->result                            = memory_backing_materialize(
        context->backing,
        &(const struct memory_backing_materialize_request){.offset = context->granule, .size = context->granule});
	return NULL;
}

Test(memory_backing, concurrent_materialization_publishes_one_extent) {
	enum { THREAD_COUNT = 16 };
	struct memory_backing*            backing;
	struct memory_backing_span        span;
	struct materialize_thread_context contexts[THREAD_COUNT];
	pthread_t                         threads[THREAD_COUNT];
	backing_test_pmm_init();
	size_t granule = backing_test_granule();
	cr_assert(memory_backing_create_anonymous(4u * granule, &backing));
	size_t before = pmm_free_size();
	for (size_t i = 0u; i < THREAD_COUNT; i++) {
		contexts[i] = (struct materialize_thread_context){.backing = backing, .granule = granule};
		cr_assert_eq(pthread_create(&threads[i], NULL, materialize_worker, &contexts[i]), 0);
	}
	for (size_t i = 0u; i < THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0);
		cr_assert(contexts[i].result);
	}
	cr_assert_eq(before - pmm_free_size(), granule);
	cr_assert(memory_backing_query(backing, granule, granule, &span));
	cr_assert_eq(span.kind, MEMORY_BACKING_SPAN_PRESENT);
	cr_assert_eq(span.size, granule);
	memory_backing_release(backing);
}
