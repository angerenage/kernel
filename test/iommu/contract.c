#include <criterion/criterion.h>
#include <hal/iommu.h>
#include <stdint.h>

static struct hal_iommu_controller_descriptor iommu_descriptor(void) {
	return (struct hal_iommu_controller_descriptor){
		.kind      = HAL_IOMMU_KIND_INTEL_VTD,
		.mock_info = {.minimum_leaf_size     = 4096u,
	                  .leaf_size_mask        = (1ull << 12u) | (1ull << 21u),
	                  .io_address_bits       = 39u,
	                  .physical_address_bits = 48u,
	                  .context_id_bits       = 8u,
	                  .source_id_bits        = 8u}
    };
}

Test(iommu, controller_geometry_and_lifecycle) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_info                  info;

	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert_eq(info.minimum_leaf_size, 4096u);
	cr_assert_eq(info.kind, HAL_IOMMU_KIND_INTEL_VTD);
	cr_assert_str_eq(hal_iommu_kind_string(HAL_IOMMU_KIND_INTEL_VTD), "Intel VT-d");
	cr_assert((info.leaf_size_mask & (1ull << 12u)) != 0u);
	cr_assert((info.leaf_size_mask & (1ull << 21u)) != 0u);
	cr_assert(!hal_iommu_mapping_supported(&controller, 0u));
	cr_assert(hal_iommu_mapping_supported(&controller, HAL_IOMMU_READ));
	cr_assert(hal_iommu_mapping_supported(&controller, HAL_IOMMU_WRITE));
	cr_assert(!hal_iommu_mapping_supported(&controller, 1ull << 8u));
	hal_iommu_controller_deinit(&controller);
	cr_assert(!controller.initialized);
}

Test(iommu, full_source_width_does_not_require_a_dense_table) {
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_space_state           space      = {0};
	struct hal_iommu_info                  info;

	descriptor.mock_info.source_id_bits = 32u;
	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert(hal_iommu_space_init(&controller, 1u, &space));
	cr_assert(hal_iommu_attach(&controller, &space, UINT32_MAX));
	cr_assert(hal_iommu_detach(&controller, UINT32_MAX));
	hal_iommu_space_deinit(&controller, &space);
	hal_iommu_controller_deinit(&controller);
}

Test(iommu, invalid_geometry_is_rejected) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_info                  info;

	descriptor.mock_info.minimum_leaf_size = 6000u;
	cr_assert(!hal_iommu_controller_init(&controller, &descriptor, &info));
	descriptor = iommu_descriptor();
	descriptor.mock_info.leaf_size_mask &= ~(1ull << 12u);
	cr_assert(!hal_iommu_controller_init(&controller, &descriptor, &info));
}

Test(iommu, spaces_validate_context_and_teardown_cleanly) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_space_state           space      = {0};
	struct hal_iommu_info                  info;

	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert(!hal_iommu_space_init(&controller, 256u, &space));
	cr_assert(hal_iommu_space_init(&controller, 255u, &space));
	cr_assert_eq(controller.live_spaces, 1u);
	hal_iommu_space_deinit(&controller, &space);
	cr_assert_eq(controller.live_spaces, 0u);
	cr_assert(!space.table.initialized);
	hal_iommu_controller_deinit(&controller);
}

Test(iommu, map_validates_ranges_and_chooses_large_leaves) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_space_state           space      = {0};
	struct hal_iommu_info                  info;
	struct hal_iommu_map_request           request = {.io_address       = 0x200000u,
	                                                  .physical_address = 0x400000u,
	                                                  .size             = 0x201000u,
	                                                  .access           = HAL_IOMMU_READ | HAL_IOMMU_WRITE};

	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert(hal_iommu_space_init(&controller, 1u, &space));
	cr_assert(hal_iommu_map(&controller, &space, &request));
	cr_assert_eq(space.leaf_count, 2u);
	cr_assert_eq(space.table.mapped_size, request.size);
	cr_assert(!hal_iommu_map(&controller, &space, &request));

	struct hal_iommu_map_request invalid = request;
	invalid.io_address++;
	cr_assert(!hal_iommu_map(&controller, &space, &invalid));
	invalid = request;
	invalid.physical_address++;
	cr_assert(!hal_iommu_map(&controller, &space, &invalid));
	invalid        = request;
	invalid.access = 1ull << 12u;
	cr_assert(!hal_iommu_map(&controller, &space, &invalid));
	invalid            = request;
	invalid.io_address = 1ull << info.io_address_bits;
	cr_assert(!hal_iommu_map(&controller, &space, &invalid));
	invalid                  = request;
	invalid.physical_address = (uintptr_t)1ull << info.physical_address_bits;
	cr_assert(!hal_iommu_map(&controller, &space, &invalid));

	cr_assert(hal_iommu_unmap(&controller, &space, request.io_address, request.size));
	cr_assert_eq(space.leaf_count, 0u);
	hal_iommu_space_deinit(&controller, &space);
	hal_iommu_controller_deinit(&controller);
}

Test(iommu, partial_large_unmap_and_hole_failure_are_atomic) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_space_state           space      = {0};
	struct hal_iommu_info                  info;

	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert(hal_iommu_space_init(&controller, 2u, &space));
	cr_assert(hal_iommu_map(
		&controller,
		&space,
		&(const struct hal_iommu_map_request){
			.io_address = 0x200000u, .physical_address = 0x800000u, .size = 0x200000u, .access = HAL_IOMMU_READ}));
	cr_assert_eq(space.leaf_count, 1u);
	cr_assert(hal_iommu_unmap(&controller, &space, 0x201000u, 0x1000u));
	cr_assert_eq(space.table.mapped_size, 0x1ff000u);
	cr_assert(!hal_iommu_unmap(&controller, &space, 0x200000u, 0x200000u));
	cr_assert_eq(space.table.mapped_size, 0x1ff000u);
	cr_assert(hal_iommu_unmap(&controller, &space, 0x200000u, 0x1000u));
	cr_assert(hal_iommu_unmap(&controller, &space, 0x202000u, 0x1fe000u));
	cr_assert_eq(space.table.mapped_size, 0u);
	hal_iommu_space_deinit(&controller, &space);
	hal_iommu_controller_deinit(&controller);
}

Test(iommu, attach_never_steals_and_detach_blocks) {
	struct hal_iommu_controller_state      controller = {0};
	struct hal_iommu_controller_descriptor descriptor = iommu_descriptor();
	struct hal_iommu_space_state           first      = {0};
	struct hal_iommu_space_state           second     = {0};
	struct hal_iommu_info                  info;

	cr_assert(hal_iommu_controller_init(&controller, &descriptor, &info));
	cr_assert(hal_iommu_space_init(&controller, 3u, &first));
	cr_assert(hal_iommu_space_init(&controller, 4u, &second));
	cr_assert(hal_iommu_attach(&controller, &first, 7u));
	cr_assert(hal_iommu_attach(&controller, &first, 7u));
	cr_assert(!hal_iommu_attach(&controller, &second, 7u));
	cr_assert(!hal_iommu_attach(&controller, &first, 256u));
	cr_assert(!hal_iommu_detach(&controller, 8u));
	cr_assert(hal_iommu_detach(&controller, 7u));
	cr_assert(hal_iommu_attach(&controller, &second, 7u));
	cr_assert(hal_iommu_detach(&controller, 7u));
	hal_iommu_space_deinit(&controller, &first);
	hal_iommu_space_deinit(&controller, &second);
	hal_iommu_controller_deinit(&controller);
}
