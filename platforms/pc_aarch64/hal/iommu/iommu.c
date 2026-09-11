#include <hal/iommu.h>
#include <string.h>

#include "../../../iommu_acpi.h"
#include "../../../iommu_fdt.h"
#include "smmuv3.h"

static size_t smmuv3_acpi_controllers(size_t target, uintptr_t* out_address) {
	const struct iommu_acpi_header* table = iommu_acpi_table("IORT");
	if (table == NULL || table->length < 48u) return 0u;
	uint32_t node_count;
	uint32_t node_offset;
	memcpy(&node_count, (const uint8_t*)table + 36u, sizeof(node_count));
	memcpy(&node_offset, (const uint8_t*)table + 40u, sizeof(node_offset));
	if (node_offset < 48u || node_offset > table->length) return 0u;
	const uint8_t* cursor = (const uint8_t*)table + node_offset;
	const uint8_t* end    = (const uint8_t*)table + table->length;
	size_t         count  = 0u;
	for (uint32_t node = 0u; node < node_count; node++) {
		if ((size_t)(end - cursor) < 4u) return count;
		uint16_t length;
		memcpy(&length, cursor + 1u, sizeof(length));
		if (length < 16u || (size_t)(end - cursor) < length) return count;
		if (cursor[0] == 4u && length >= 24u) {
			uint64_t address;
			memcpy(&address, cursor + 16u, sizeof(address));
			if (address != 0u && address <= UINTPTR_MAX) {
				if (out_address != NULL && count == target) *out_address = (uintptr_t)address;
				count++;
			}
		}
		cursor += length;
	}
	return count;
}

size_t hal_iommu_controller_count(void) {
	return iommu_fdt_controllers("arm,smmu-v3", SIZE_MAX, NULL) + smmuv3_acpi_controllers(SIZE_MAX, NULL);
}

bool hal_iommu_controller_at(size_t index, struct hal_iommu_controller_descriptor* out_descriptor) {
	uintptr_t address;
	if (out_descriptor == NULL) return false;
	size_t count = iommu_fdt_controllers("arm,smmu-v3", index, &address);
	if (index >= count && index - count >= smmuv3_acpi_controllers(index - count, &address)) return false;
	*out_descriptor =
		(struct hal_iommu_controller_descriptor){.kind = HAL_IOMMU_KIND_ARM_SMMUV3, .register_address = address};
	return true;
}

bool hal_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                               const struct hal_iommu_controller_descriptor* descriptor,
                               struct hal_iommu_info*                        out_info) {
	return descriptor != NULL && descriptor->kind == HAL_IOMMU_KIND_ARM_SMMUV3 &&
	       aarch64_smmuv3_controller_init(controller, descriptor, out_info);
}

void hal_iommu_controller_deinit(struct hal_iommu_controller_state* controller) {
	aarch64_smmuv3_controller_deinit(controller);
}

bool hal_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	return aarch64_smmuv3_mapping_supported(controller, access);
}

bool hal_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                          struct hal_iommu_space_state* space) {
	return aarch64_smmuv3_space_init(controller, context_id, space);
}

void hal_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	aarch64_smmuv3_space_deinit(controller, space);
}

bool hal_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request) {
	return aarch64_smmuv3_map(controller, space, request);
}

bool hal_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     uint64_t io_address, size_t size) {
	return aarch64_smmuv3_unmap(controller, space, io_address, size);
}

bool hal_iommu_protect(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                       uint64_t io_address, size_t size, uint64_t access) {
	return aarch64_smmuv3_protect(controller, space, io_address, size, access);
}

bool hal_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                      uint32_t source_id, struct hal_iommu_attachment_state* attachment) {
	return aarch64_smmuv3_attach(controller, space, source_id, attachment);
}

bool hal_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                      struct hal_iommu_attachment_state* attachment) {
	return aarch64_smmuv3_detach(controller, source_id, attachment);
}
