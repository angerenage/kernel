#include <base/dma.h>
#include <base/heap.h>
#include <core/address_space.h>
#include <core/address_transfer.h>
#include <core/dma.h>
#include <core/memory.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <hal/iommu.h>
#include <stdint.h>
#include <stdlib.h>

extern void   hal_iommu_mock_set_discovered_controllers(const struct hal_iommu_controller_descriptor* descriptors,
                                                        size_t                                        count);
extern void   hal_iommu_mock_reset_discovered_controllers(void);
extern void   hal_iommu_mock_fail_map_after(size_t successful_maps);
extern bool   dma_test_init_pmm(void);
extern void   mock_cache_reset(void);
extern size_t mock_cache_dma_device_sync_count(void);
extern size_t mock_cache_dma_cpu_sync_count(void);
extern size_t mock_cache_dma_device_sync_bytes(void);
extern size_t mock_cache_dma_cpu_sync_bytes(void);

static void dma_test_prepare(void) {
	cr_assert(dma_test_init_pmm(), "pmm_init failed");
	if (!heap_is_initialized()) cr_assert(heap_init(), "heap_init failed");
	cr_assert(address_space_init(), "address_space_init failed");
	hal_iommu_mock_reset_discovered_controllers();
	mock_cache_reset();
}

Test(dma, hal_iommu_controller_count_test) {
	dma_test_prepare();
	size_t count = hal_iommu_controller_count();
	cr_assert(count == 2, "Expected 2 mock IOMMU controllers, got %zu", count);
}

Test(dma, mock_iommu_check) {
	dma_test_prepare();
	size_t count = hal_iommu_controller_count();
	cr_assert(count == 2, "Expected 2 mock IOMMU controllers, got %zu", count);
	struct hal_iommu_controller_descriptor desc;
	cr_assert(hal_iommu_controller_at(0, &desc));
	cr_assert(desc.register_address == 0x1000u);
	cr_assert(hal_iommu_controller_at(1, &desc));
	cr_assert(desc.register_address == 0x2000u);
}

Test(dma, init) {
	dma_test_prepare();
	cr_assert(dma_init());
}

Test(dma, source_resolution_and_lazy_init) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	uint32_t     local_source_id = 5;

	cr_assert(dma_source_resolve(0x1000u, local_source_id, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	cr_assert_not_null(space);
	cr_assert(space->backend.device.context_id != 0u);
	struct dma_binding* binding;
	cr_assert(dma_bind(source, space, &binding));
	cr_assert_not_null(binding);
	cr_assert(dma_unbind(binding));
	dma_binding_release(binding);
	address_space_device_release(space);
}

Test(dma, context_uniqueness_and_reuse) {
	dma_test_prepare();
	cr_assert(dma_init());

#define NUM_SPACES 8
	struct address_space* spaces[NUM_SPACES];
	uint32_t              context_ids[NUM_SPACES];
	size_t                i;

	for (i = 0; i < NUM_SPACES; i++) {
		dma_source_t source;
		cr_assert(dma_source_resolve(0x1000u, i, &source));
		cr_assert(dma_address_space_create(source, &spaces[i]));
		cr_assert_not_null(spaces[i]);
		context_ids[i] = spaces[i]->backend.device.context_id;
	}

	for (size_t j = 0; j < NUM_SPACES; j++) {
		for (size_t k = j + 1; k < NUM_SPACES; k++) {
			cr_assert_neq(context_ids[j], context_ids[k]);
		}
	}

	address_space_device_release(spaces[0]);
	spaces[0] = NULL;

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 0, &source));
	struct address_space* extra_space;
	cr_assert(dma_address_space_create(source, &extra_space));
	cr_assert_not_null(extra_space);

	bool found = false;
	for (size_t j = 0; j < NUM_SPACES; j++) {
		if (extra_space->backend.device.context_id == context_ids[j]) {
			found = true;
			break;
		}
	}
	cr_assert(found, "Reused context ID %u not found in previously used set", extra_space->backend.device.context_id);

	for (i = 1; i < NUM_SPACES; i++) {
		if (spaces[i]) {
			address_space_device_release(spaces[i]);
		}
	}
	if (extra_space) {
		address_space_device_release(extra_space);
	}
}

Test(dma, context_exhaustion_is_fatal) {
	dma_test_prepare();
	cr_assert(dma_init());

	const uint32_t CONTEXTS_PER_CONTROLLER = 255u;
	const uint32_t CONTROLLER_COUNT        = 2u;
	const uint32_t TOTAL_CONTEXTS          = CONTEXTS_PER_CONTROLLER * CONTROLLER_COUNT;

	struct address_space* spaces[TOTAL_CONTEXTS];
	dma_source_t          sources[TOTAL_CONTEXTS];
	uint32_t              context_ids[TOTAL_CONTEXTS];
	uint32_t              controller_indices[TOTAL_CONTEXTS];
	uint32_t              local_source_ids[TOTAL_CONTEXTS];

	for (uint32_t i = 0; i < TOTAL_CONTEXTS; i++) {
		controller_indices[i] = i % CONTROLLER_COUNT;
		local_source_ids[i]   = i % 255;

		cr_assert(dma_source_resolve(controller_indices[i] == 0 ? 0x1000u : 0x2000u, local_source_ids[i], &sources[i]),
		          "Failed to resolve source for context %u",
		          i);

		cr_assert(dma_address_space_create(sources[i], &spaces[i]), "Failed to create address space for context %u", i);
		cr_assert_not_null(spaces[i], "Null address space for context %u", i);

		context_ids[i] = spaces[i]->backend.device.context_id;
		cr_assert(context_ids[i] != 0, "Context ID 0 is reserved but was allocated for context %u", i);
		cr_assert_leq(context_ids[i],
		              CONTEXTS_PER_CONTROLLER,
		              "Context ID %u exceeds mock controller limit for controller %u",
		              context_ids[i],
		              controller_indices[i]);
	}

	for (uint32_t c = 0; c < CONTROLLER_COUNT; c++) {
		bool used_contexts[256] = {false}; // 8 bits = 256 possible values
		for (uint32_t i = 0; i < TOTAL_CONTEXTS; i++) {
			if (controller_indices[i] == c) {
				cr_assert(!used_contexts[context_ids[i]],
				          "Duplicate context ID %u detected for controller %u",
				          context_ids[i],
				          c);
				used_contexts[context_ids[i]] = true;
			}
		}
	}

	dma_source_t          overflow_source;
	struct address_space* overflow_space = NULL;
	bool                  resolve_ok     = dma_source_resolve(0x1000u, 0, &overflow_source);
	bool                  create_ok      = resolve_ok && dma_address_space_create(overflow_source, &overflow_space);
	cr_assert_not(create_ok, "Expected context allocation to fail after exhausting all contexts");
	cr_assert_null(overflow_space);

	address_space_device_release(spaces[0]);
	spaces[0] = NULL;

	struct address_space* reclaimed_space;
	cr_assert(dma_address_space_create(sources[0], &reclaimed_space), "Failed to reclaim context after release");
	cr_assert_not_null(reclaimed_space, "Null reclaimed address space");
	cr_assert(reclaimed_space->backend.device.context_id == context_ids[0],
	          "Reclaimed context ID %u does not match original %u",
	          reclaimed_space->backend.device.context_id,
	          context_ids[0]);
	address_space_device_release(reclaimed_space);

	for (uint32_t i = 0; i < TOTAL_CONTEXTS; i++) {
		if (spaces[i]) {
			address_space_device_release(spaces[i]);
			spaces[i] = NULL;
		}
	}

	bool reused_contexts[2][256] = {{false}};
	for (uint32_t i = 0; i < TOTAL_CONTEXTS; i++) {
		cr_assert(dma_address_space_create(sources[i], &spaces[i]),
		          "Failed to re-create address space for context %u after full release",
		          i);
		cr_assert_not_null(spaces[i], "Null address space for context %u after full release", i);
		context_ids[i] = spaces[i]->backend.device.context_id;
		cr_assert(context_ids[i] != 0, "Context ID 0 is reserved but was allocated for context %u", i);
		cr_assert_not(reused_contexts[controller_indices[i]][context_ids[i]],
		              "Context ID %u was reused before its prior allocation was released",
		              context_ids[i]);
		reused_contexts[controller_indices[i]][context_ids[i]] = true;
	}

	for (uint32_t i = 0; i < TOTAL_CONTEXTS; i++) {
		if (spaces[i]) {
			address_space_device_release(spaces[i]);
		}
	}
}

Test(dma, duplicate_bind_fails) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 0, &source));

	struct address_space* space1;
	struct address_space* space2;
	cr_assert(dma_address_space_create(source, &space1));
	cr_assert(dma_address_space_create(source, &space2));

	struct dma_binding* binding1;
	struct dma_binding* binding2;

	cr_assert(dma_bind(source, space1, &binding1));
	cr_assert_not_null(binding1);
	cr_assert(!dma_bind(source, space2, &binding2));
	cr_assert_null(binding2);
	cr_assert(!dma_bind(source, space1, &binding2));
	cr_assert_null(binding2);

	if (binding1) {
		cr_assert(dma_unbind(binding1));
		dma_binding_release(binding1);
	}
	address_space_device_release(space1);
	address_space_device_release(space2);
}

Test(dma, bind_rejects_source_outside_controller_width) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t valid_source;
	dma_source_t invalid_source;
	cr_assert(dma_source_resolve(0x1000u, 1u, &valid_source));
	cr_assert(dma_source_resolve(0x1000u, 256u, &invalid_source));

	struct address_space* space;
	cr_assert(dma_address_space_create(valid_source, &space));

	struct dma_binding* binding = NULL;
	cr_assert_not(dma_bind(invalid_source, space, &binding));
	cr_assert_null(binding);

	address_space_device_release(space);
}

Test(dma, recover_after_reference_drop) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 0, &source));

	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	cr_assert_not_null(space);

	struct dma_binding* binding;
	cr_assert(dma_bind(source, space, &binding));
	cr_assert_not_null(binding);
	dma_binding_release(binding);

	struct dma_binding* recovered;
	cr_assert(dma_binding_recover(source, &recovered));
	cr_assert_not_null(recovered);
	cr_assert_eq(dma_binding_source(recovered), source);

	dma_binding_release(recovered);
	address_space_device_release(space);
}

Test(dma, unbind_then_recover_fails) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 0, &source));

	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	cr_assert_not_null(space);

	struct dma_binding* binding;
	cr_assert(dma_bind(source, space, &binding));
	cr_assert_not_null(binding);
	cr_assert(dma_unbind(binding));

	struct dma_binding* recovered;
	cr_assert(!dma_binding_recover(source, &recovered));
	cr_assert_null(recovered);

	dma_binding_release(binding);
	address_space_device_release(space);
}

Test(dma, binding_address_space_lifetime) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 0, &source));

	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	cr_assert_not_null(space);

	struct dma_binding* binding;
	cr_assert(dma_bind(source, space, &binding));
	cr_assert_not_null(binding);

	struct address_space* space_from_binding = dma_binding_address_space(binding);
	cr_assert_not_null(space_from_binding);
	cr_assert_eq(space_from_binding, space);
	address_space_device_release(space_from_binding);

	address_space_device_release(space);
	space              = NULL;
	space_from_binding = dma_binding_address_space(binding);
	cr_assert_not_null(space_from_binding);
	address_space_device_release(space_from_binding);

	cr_assert(dma_unbind(binding));
	cr_assert_null(dma_binding_address_space(binding));
	dma_binding_release(binding);
}

Test(dma, device_mapping_none_rw_protect_and_unmap) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 1u, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	size_t granule = address_space_minimum_mapping_size(space);
	cr_assert_neq(granule, 0u);

	struct memory* memory;
	cr_assert(memory_create_anonymous(2u * granule, &memory));
	struct mapping* mapping;
	cr_assert(address_space_map(
		space, &(const struct address_space_mapping_request){.memory = memory, .access = 0u}, &mapping));
	cr_assert_eq(mapping_access(mapping), 0u);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 0u);

	cr_assert(address_space_protect(space, mapping, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE));
	cr_assert_eq(mapping_access(mapping), MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 2u * granule);
	cr_assert_eq(address_space_validate_range(space, mapping_address(mapping), granule, ADDRESS_TRANSFER_READ),
	             ADDRESS_TRANSFER_INVALID_ARGUMENTS);

	cr_assert(address_space_protect(space, mapping, MEMORY_ACCESS_READ));
	cr_assert_eq(mapping_access(mapping), MEMORY_ACCESS_READ);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 2u * granule);
	cr_assert_not(address_space_protect(space, mapping, MEMORY_ACCESS_EXEC));
	cr_assert_eq(mapping_access(mapping), MEMORY_ACCESS_READ);

	cr_assert(address_space_protect(space, mapping, 0u));
	cr_assert_eq(mapping_access(mapping), 0u);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 0u);
	cr_assert(address_space_unmap(space, mapping));
	mapping_release(mapping);
	memory_release(memory);
	address_space_device_release(space);
}

Test(dma, device_none_reservation_defers_physical_compatibility) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 2u, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	size_t granule = address_space_minimum_mapping_size(space);
	cr_assert_neq(granule, 0u);

	struct memory* memory;
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address        = (uintptr_t)1ull << 52u,
			.size                    = granule,
			.memory_type             = MEMORY_TYPE_NORMAL,
			.external_cpu_accessible = false,
		},
		&memory));
	struct mapping* mapping;
	cr_assert(address_space_map(
		space, &(const struct address_space_mapping_request){.memory = memory, .access = 0u}, &mapping));
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 0u);
	cr_assert_not(address_space_protect(space, mapping, MEMORY_ACCESS_READ));
	cr_assert_eq(mapping_access(mapping), 0u);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 0u);

	cr_assert(address_space_unmap(space, mapping));
	mapping_release(mapping);
	memory_release(memory);
	address_space_device_release(space);
}

Test(dma, device_fragmented_map_failure_rolls_back_prefix) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 3u, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	size_t granule = address_space_minimum_mapping_size(space);
	cr_assert_neq(granule, 0u);

	struct memory* memory;
	cr_assert(memory_create_anonymous(2u * granule, &memory));
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .offset = 0u, .size = granule, .alignment = granule, .require_contiguous = true}));

	struct memory_span first;
	cr_assert(memory_query(memory, 0u, granule, &first));
	cr_assert_eq(first.kind, MEMORY_SPAN_PRESENT);
	cr_assert_leq(first.physical_address, UINTPTR_MAX - 2u * granule);

	uintptr_t second_minimum = first.physical_address + 2u * granule;
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .offset             = granule,
									 .size               = granule,
									 .alignment          = granule,
									 .minimum_address    = second_minimum,
									 .require_contiguous = true,
								 }));

	struct memory_span second;
	cr_assert(memory_query(memory, granule, granule, &second));
	cr_assert_eq(second.kind, MEMORY_SPAN_PRESENT);
	cr_assert_neq(second.physical_address, first.physical_address + granule);

	hal_iommu_mock_fail_map_after(1u);
	struct mapping* mapping = NULL;
	cr_assert_not(
		address_space_map(space,
	                      &(const struct address_space_mapping_request){.memory = memory, .access = MEMORY_ACCESS_READ},
	                      &mapping));
	cr_assert_null(mapping);
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 0u);
	hal_iommu_mock_fail_map_after(SIZE_MAX);

	memory_release(memory);
	address_space_device_release(space);
}

Test(dma, device_materialization_uses_iommu_granules) {
	dma_test_prepare();
	const size_t                           iommu_granule = 16u * 1024u;
	struct hal_iommu_controller_descriptor descriptor    = {
		   .kind             = HAL_IOMMU_KIND_INTEL_VTD,
		   .register_address = 0x3000u,
		   .mock_info =
            {
						.minimum_leaf_size     = iommu_granule,
						.leaf_size_mask        = 1ull << 14u,
						.io_address_bits       = 39u,
						.physical_address_bits = 48u,
						.context_id_bits       = 8u,
						.source_id_bits        = 8u,
						},
    };
	hal_iommu_mock_set_discovered_controllers(&descriptor, 1u);
	cr_assert(dma_init());
	cr_assert_lt(pmm_info()->allocation_granule, iommu_granule);

	dma_source_t source;
	cr_assert(dma_source_resolve(descriptor.register_address, 1u, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	cr_assert_eq(address_space_minimum_mapping_size(space), iommu_granule);

	struct memory* reserved_memory;
	struct memory* eager_memory;
	cr_assert(memory_create_anonymous(2u * iommu_granule, &reserved_memory));
	cr_assert(memory_create_anonymous(2u * iommu_granule, &eager_memory));
	struct mapping* reserved_mapping;
	cr_assert(address_space_map(space,
	                            &(const struct address_space_mapping_request){.memory = reserved_memory, .access = 0u},
	                            &reserved_mapping));

	struct pmm_extent held[256]  = {0};
	size_t            held_count = 0u;
	while (held_count < sizeof(held) / sizeof(held[0]) &&
	       pmm_alloc(&(const struct pmm_alloc_request){.size = iommu_granule, .alignment = iommu_granule},
	                 &held[held_count]))
		held_count++;
	cr_assert_geq(held_count, 8u);
	for (size_t i = 0u; i < held_count; i++)
		for (size_t j = i + 1u; j < held_count; j++)
			if (held[j].address < held[i].address) {
				struct pmm_extent tmp = held[i];
				held[i]               = held[j];
				held[j]               = tmp;
			}
	for (size_t i = 0u; i < held_count; i += 2u) {
		cr_assert(pmm_free(held[i]));
		held[i] = (struct pmm_extent){0};
	}

	struct pmm_extent large_blockers[128] = {0};
	size_t            large_count         = 0u;
	while (large_count < sizeof(large_blockers) / sizeof(large_blockers[0]) &&
	       pmm_alloc(&(const struct pmm_alloc_request){.size = 2u * iommu_granule, .alignment = iommu_granule},
	                 &large_blockers[large_count]))
		large_count++;
	struct pmm_extent probe[4];
	for (size_t i = 0u; i < sizeof(probe) / sizeof(probe[0]); i++)
		cr_assert(
			pmm_alloc(&(const struct pmm_alloc_request){.size = iommu_granule, .alignment = iommu_granule}, &probe[i]));
	for (size_t i = 0u; i < sizeof(probe) / sizeof(probe[0]); i++) cr_assert(pmm_free(probe[i]));
	struct pmm_extent unavailable;
	cr_assert_not(pmm_alloc(&(const struct pmm_alloc_request){.size = 2u * iommu_granule, .alignment = iommu_granule},
	                        &unavailable));

	struct mapping* eager_mapping;
	cr_assert(address_space_map(space,
	                            &(const struct address_space_mapping_request){
									.memory = eager_memory, .access = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE},
	                            &eager_mapping));
	cr_assert(address_space_protect(space, reserved_mapping, MEMORY_ACCESS_READ));
	cr_assert_eq(space->backend.device.hal.table.mapped_size, 4u * iommu_granule);

	struct memory_span first, second;
	cr_assert(memory_query(eager_memory, 0u, iommu_granule, &first));
	cr_assert(memory_query(eager_memory, iommu_granule, iommu_granule, &second));
	cr_assert_eq(first.kind, MEMORY_SPAN_PRESENT);
	cr_assert_eq(second.kind, MEMORY_SPAN_PRESENT);
	cr_assert_eq(first.size, iommu_granule);
	cr_assert_neq(second.physical_address, first.physical_address + iommu_granule);

	cr_assert(address_space_unmap(space, eager_mapping));
	cr_assert(address_space_unmap(space, reserved_mapping));
	mapping_release(eager_mapping);
	mapping_release(reserved_mapping);
	memory_release(eager_memory);
	memory_release(reserved_memory);
	address_space_device_release(space);
	for (size_t i = 0u; i < held_count; i++)
		if (held[i].size != 0u) cr_assert(pmm_free(held[i]));
	for (size_t i = 0u; i < large_count; i++) cr_assert(pmm_free(large_blockers[i]));
}

Test(dma, mapping_sync_supports_aligned_subranges) {
	dma_test_prepare();
	cr_assert(dma_init());

	dma_source_t source;
	cr_assert(dma_source_resolve(0x1000u, 4u, &source));
	struct address_space* space;
	cr_assert(dma_address_space_create(source, &space));
	size_t granule = address_space_minimum_mapping_size(space);
	cr_assert_neq(granule, 0u);

	struct memory* memory;
	cr_assert(memory_create_anonymous(2u * granule, &memory));
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .offset             = 0u,
									 .size               = granule,
									 .alignment          = granule,
									 .require_contiguous = true,
								 }));
	struct memory_span first;
	cr_assert(memory_query(memory, 0u, granule, &first));
	cr_assert_eq(first.kind, MEMORY_SPAN_PRESENT);
	cr_assert_leq(first.physical_address, UINTPTR_MAX - 2u * granule);
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .offset             = granule,
									 .size               = granule,
									 .alignment          = granule,
									 .minimum_address    = first.physical_address + 2u * granule,
									 .require_contiguous = true,
								 }));
	struct memory_span second;
	cr_assert(memory_query(memory, granule, granule, &second));
	cr_assert_eq(second.kind, MEMORY_SPAN_PRESENT);
	cr_assert_neq(second.physical_address, first.physical_address + granule);

	struct mapping* mapping;
	cr_assert(address_space_map(space,
	                            &(const struct address_space_mapping_request){
									.memory = memory, .access = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE},
	                            &mapping));

	mock_cache_reset();
	cr_assert(dma_mapping_sync(space, mapping, 0u, 2u * granule, DMA_SYNC_FOR_DEVICE));
	cr_assert_eq(mock_cache_dma_device_sync_count(), 2u);
	cr_assert_eq(mock_cache_dma_device_sync_bytes(), 2u * granule);
	cr_assert_eq(mock_cache_dma_cpu_sync_count(), 0u);

	mock_cache_reset();
	cr_assert(dma_mapping_sync(space, mapping, granule, granule, DMA_SYNC_FOR_CPU));
	cr_assert_eq(mock_cache_dma_cpu_sync_count(), 1u);
	cr_assert_eq(mock_cache_dma_cpu_sync_bytes(), granule);
	cr_assert_eq(mock_cache_dma_device_sync_count(), 0u);

	cr_assert_not(dma_mapping_sync(space, mapping, 1u, granule, DMA_SYNC_FOR_CPU));
	cr_assert_not(dma_mapping_sync(space, mapping, 0u, granule - 1u, DMA_SYNC_FOR_CPU));
	cr_assert_not(dma_mapping_sync(space, mapping, 0u, 0u, DMA_SYNC_FOR_CPU));
	cr_assert_not(dma_mapping_sync(space, mapping, 2u * granule, granule, DMA_SYNC_FOR_CPU));
	cr_assert_not(dma_mapping_sync(space, mapping, 0u, granule, (enum dma_sync_target)99u));

	cr_assert(address_space_protect(space, mapping, 0u));
	cr_assert_not(dma_mapping_sync(space, mapping, 0u, granule, DMA_SYNC_FOR_DEVICE));

	cr_assert(address_space_unmap(space, mapping));
	mapping_release(mapping);
	memory_release(memory);
	address_space_device_release(space);
}
