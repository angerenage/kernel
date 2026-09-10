#include <core/pmm.h>
#include <hal/iommu.h>
#include <kernel/cmdline.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"

static bool kernel_selftest_iommu_kind_valid(enum hal_iommu_kind kind) {
	switch (kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
	case HAL_IOMMU_KIND_AMD:
	case HAL_IOMMU_KIND_ARM_SMMUV3:
	case HAL_IOMMU_KIND_RISCV:
	case HAL_IOMMU_KIND_LOONGARCH_V1:
		return true;
	}
	return false;
}

static bool kernel_selftest_iommu_expected_kind(const char* value, size_t value_len, enum hal_iommu_kind* out_kind,
                                                bool* out_none) {
	if (out_kind == NULL || out_none == NULL) return false;
	*out_none = false;
	if (kernel_cmdline_value_equals(value, value_len, "none")) {
		*out_none = true;
		return true;
	}
	if (kernel_cmdline_value_equals(value, value_len, "vtd")) *out_kind = HAL_IOMMU_KIND_INTEL_VTD;
	else if (kernel_cmdline_value_equals(value, value_len, "amd")) *out_kind = HAL_IOMMU_KIND_AMD;
	else if (kernel_cmdline_value_equals(value, value_len, "smmuv3")) *out_kind = HAL_IOMMU_KIND_ARM_SMMUV3;
	else if (kernel_cmdline_value_equals(value, value_len, "riscv")) *out_kind = HAL_IOMMU_KIND_RISCV;
	else if (kernel_cmdline_value_equals(value, value_len, "loongarch-v1")) *out_kind = HAL_IOMMU_KIND_LOONGARCH_V1;
	else return false;
	return true;
}

static unsigned kernel_selftest_iommu_size_shift(size_t size) {
	unsigned shift = 0u;
	while (((size_t)1u << shift) != size) shift++;
	return shift;
}

static size_t kernel_selftest_iommu_next_leaf(const struct hal_iommu_info* info, size_t allocation_granule) {
	unsigned minimum_shift = kernel_selftest_iommu_size_shift(info->minimum_leaf_size);
	for (unsigned shift = minimum_shift + 1u; shift < 64u && shift < sizeof(size_t) * 8u; shift++) {
		size_t leaf_size = (size_t)1u << shift;
		if (leaf_size >= allocation_granule && (info->leaf_size_mask & (1ull << shift)) != 0u) return leaf_size;
	}
	return 0u;
}

static uintptr_t kernel_selftest_iommu_physical_limit(uint8_t bits) {
	return bits < sizeof(uintptr_t) * 8u ? (uintptr_t)1u << bits : 0u;
}

static bool kernel_selftest_iommu_io_address(size_t size, uint8_t bits, uint64_t* out_address) {
	if (out_address == NULL || bits == 0u || bits > 64u) return false;
	if (bits == 64u) {
		if (size > UINT64_MAX / 2u) return false;
		*out_address = size;
		return true;
	}
	uint64_t limit = 1ull << bits;
	if (size > limit) return false;
	*out_address = size <= limit - size ? size : 0u;
	return true;
}

static bool kernel_selftest_iommu_allocate(const struct hal_iommu_info* info, size_t size,
                                           struct pmm_extent* out_extent) {
	const struct pmm_info* pmm       = pmm_info();
	size_t                 alignment = size;
	if (pmm == NULL) return false;
	if (alignment < pmm->allocation_granule) alignment = pmm->allocation_granule;
	return pmm_alloc(&(const struct pmm_alloc_request){.size            = size,
	                                                   .alignment       = alignment,
	                                                   .maximum_address = kernel_selftest_iommu_physical_limit(
														   info->physical_address_bits)},
	                 out_extent);
}

static void kernel_selftest_iommu_exercise_controller(struct kernel_selftest_context*               ctx,
                                                      const struct hal_iommu_controller_descriptor* descriptor) {
	const struct pmm_info*            pmm        = pmm_info();
	struct hal_iommu_controller_state controller = {0};
	struct hal_iommu_space_state      space      = {0};
	struct hal_iommu_info             info       = {0};
	struct pmm_extent                 extent     = {0};
	size_t                            free_before;
	size_t                            map_size       = 0u;
	uint64_t                          io_address     = 0u;
	bool                              controller_up  = false;
	bool                              space_up       = false;
	bool                              mapping_exists = false;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, pmm != NULL, "pmm properties unavailable", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, hal_iommu_controller_init(&controller, descriptor, &info), "controller warm-up failed", cleanup);
	controller_up = true;
	hal_iommu_controller_deinit(&controller);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, !controller.initialized, "controller warm-up teardown failed", cleanup);
	controller_up = false;
	controller    = (struct hal_iommu_controller_state){0};
	info          = (struct hal_iommu_info){0};
	free_before   = pmm_free_size();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, hal_iommu_controller_init(&controller, descriptor, &info), "controller initialization failed", cleanup);
	controller_up = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_iommu_kind_valid(info.kind), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.kind == descriptor->kind, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.minimum_leaf_size != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (info.minimum_leaf_size & (info.minimum_leaf_size - 1u)) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, (info.leaf_size_mask & (1ull << kernel_selftest_iommu_size_shift(info.minimum_leaf_size))) != 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.io_address_bits != 0u && info.io_address_bits <= 64u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, info.physical_address_bits != 0u && info.physical_address_bits <= sizeof(uintptr_t) * 8u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.context_id_bits != 0u && info.context_id_bits <= 32u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, info.source_id_bits != 0u && info.source_id_bits <= 32u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_mapping_supported(&controller, 0u), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_mapping_supported(&controller, 1ull << 63u), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_iommu_mapping_supported(&controller, HAL_IOMMU_READ), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, hal_iommu_mapping_supported(&controller, HAL_IOMMU_READ | HAL_IOMMU_WRITE), cleanup);

	map_size = info.minimum_leaf_size > pmm->allocation_granule ? info.minimum_leaf_size : pmm->allocation_granule;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                kernel_selftest_iommu_io_address(map_size, info.io_address_bits, &io_address),
	                                "controller I/O address width cannot hold one minimum leaf",
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, hal_iommu_space_init(&controller, 1u, &space), "translation-space initialization failed", cleanup);
	space_up = true;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_iommu_allocate(&info, map_size, &extent), "minimum-leaf PMM allocation failed", cleanup);

	struct hal_iommu_map_request request = {.io_address       = io_address,
	                                        .physical_address = extent.address,
	                                        .size             = map_size,
	                                        .access           = HAL_IOMMU_READ | HAL_IOMMU_WRITE};
	struct hal_iommu_map_request invalid = request;
	invalid.io_address++;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_map(&controller, &space, &invalid), cleanup);
	invalid = request;
	invalid.physical_address++;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_map(&controller, &space, &invalid), cleanup);
	invalid = request;
	invalid.size--;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_map(&controller, &space, &invalid), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, hal_iommu_map(&controller, &space, &request), "minimum-leaf map failed", cleanup);
	mapping_exists = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_map(&controller, &space, &request), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, hal_iommu_unmap(&controller, &space, io_address, map_size), "minimum-leaf unmap failed", cleanup);
	mapping_exists = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_unmap(&controller, &space, io_address, map_size), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free(extent), cleanup);
	extent = (struct pmm_extent){0};

	map_size = kernel_selftest_iommu_next_leaf(&info, pmm->allocation_granule);
	if (map_size != 0u) {
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
		                                kernel_selftest_iommu_io_address(map_size, info.io_address_bits, &io_address),
		                                "controller I/O address width cannot hold an advertised large leaf",
		                                cleanup);
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, kernel_selftest_iommu_allocate(&info, map_size, &extent), "large-leaf PMM allocation failed", cleanup);
		request = (struct hal_iommu_map_request){
			.io_address = io_address, .physical_address = extent.address, .size = map_size, .access = HAL_IOMMU_READ};
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, hal_iommu_map(&controller, &space, &request), "large-leaf map failed", cleanup);
		mapping_exists = true;
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			hal_iommu_unmap(&controller, &space, io_address + info.minimum_leaf_size, info.minimum_leaf_size),
			"partial large-leaf unmap failed",
			cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, !hal_iommu_unmap(&controller, &space, io_address, map_size), cleanup);
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
		                                hal_iommu_unmap(&controller, &space, io_address, info.minimum_leaf_size),
		                                "large-leaf prefix cleanup failed",
		                                cleanup);
		size_t suffix_size = map_size - 2u * info.minimum_leaf_size;
		if (suffix_size != 0u)
			KERNEL_SELFTEST_ASSERT_MSG_GOTO(
				ctx,
				hal_iommu_unmap(&controller, &space, io_address + 2u * info.minimum_leaf_size, suffix_size),
				"large-leaf suffix cleanup failed",
				cleanup);
		mapping_exists = false;
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, pmm_free(extent), cleanup);
		extent = (struct pmm_extent){0};
	}

cleanup:
	if (mapping_exists && space_up) {
		(void)hal_iommu_unmap(&controller, &space, io_address, map_size);
		if (map_size > info.minimum_leaf_size) {
			(void)hal_iommu_unmap(&controller, &space, io_address, info.minimum_leaf_size);
			if (map_size > 2u * info.minimum_leaf_size)
				(void)hal_iommu_unmap(&controller,
				                      &space,
				                      io_address + 2u * info.minimum_leaf_size,
				                      map_size - 2u * info.minimum_leaf_size);
		}
	}
	if (space_up) hal_iommu_space_deinit(&controller, &space);
	if (controller_up) hal_iommu_controller_deinit(&controller);
	if (extent.size != 0u) (void)pmm_free(extent);
	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, pmm_free_size() == free_before);
}

static void kernel_selftest_iommu_discovered_controllers_obey_contract(struct kernel_selftest_context* ctx) {
	const char*         expected_value;
	size_t              expected_value_len;
	enum hal_iommu_kind expected_kind = HAL_IOMMU_KIND_INTEL_VTD;
	bool                expected_none = false;
	bool                expected_seen = false;
	bool has_expectation = kernel_cmdline_option_value("kernel.selftest.iommu", &expected_value, &expected_value_len);
	if (has_expectation)
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_iommu_expected_kind(expected_value, expected_value_len, &expected_kind, &expected_none),
			"kernel.selftest.iommu names an unsupported backend",
			done);
	size_t count = hal_iommu_controller_count();
	if (has_expectation && expected_none) {
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, count == 0u, "an IOMMU was discovered when none was expected", done);
	}
	KERNEL_SELFTEST_ASSERT(ctx, !hal_iommu_controller_at(count, &(struct hal_iommu_controller_descriptor){0}));
	for (size_t index = 0u; index < count; index++) {
		struct hal_iommu_controller_descriptor descriptor;
		KERNEL_SELFTEST_ASSERT_MSG(
			ctx, hal_iommu_controller_at(index, &descriptor), "enumerated controller has no descriptor");
		KERNEL_SELFTEST_ASSERT_MSG(
			ctx, kernel_selftest_iommu_kind_valid(descriptor.kind), "enumerated controller has an invalid kind");
		KERNEL_SELFTEST_ASSERT_MSG(ctx, descriptor.register_address != 0u, "enumerated controller has no registers");
		if (has_expectation && descriptor.kind == expected_kind) expected_seen = true;
		for (size_t previous = 0u; previous < index; previous++) {
			struct hal_iommu_controller_descriptor other;
			KERNEL_SELFTEST_ASSERT_GOTO(ctx, hal_iommu_controller_at(previous, &other), done);
			KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
			                                descriptor.register_address != other.register_address,
			                                "IOMMU controller register address is not unique",
			                                done);
		}
		kernel_selftest_iommu_exercise_controller(ctx, &descriptor);
		if (ctx->failure_expr != NULL) break;
	}
	if (has_expectation && !expected_none)
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, expected_seen, "expected IOMMU backend was not discovered and tested", done);
done:
	return;
}

static const struct kernel_selftest_case kernel_iommu_selftests[] = {
	{.name = "discovered_controllers_obey_contract", .run = kernel_selftest_iommu_discovered_controllers_obey_contract},
};

const struct kernel_selftest_suite kernel_iommu_selftest_suite = {
	.name       = "iommu",
	.cases      = kernel_iommu_selftests,
	.case_count = sizeof(kernel_iommu_selftests) / sizeof(kernel_iommu_selftests[0]),
};
