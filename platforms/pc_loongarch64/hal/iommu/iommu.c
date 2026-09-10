#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_acpi.h"
#include "v1.h"

static size_t loongarch_iommu_controllers(size_t target, struct hal_iommu_controller_descriptor* out_descriptor) {
	const struct iommu_acpi_header* table = iommu_acpi_table("IOVT");
	if (table == NULL || table->revision != 1u || table->length < 48u) return 0u;
	uint16_t structure_count;
	uint16_t structure_offset;
	memcpy(&structure_count, (const uint8_t*)table + 36u, sizeof(structure_count));
	memcpy(&structure_offset, (const uint8_t*)table + 38u, sizeof(structure_offset));
	if (structure_offset < 48u || structure_offset > table->length) return 0u;
	const uint8_t* cursor = (const uint8_t*)table + structure_offset;
	const uint8_t* end    = (const uint8_t*)table + table->length;
	size_t         count  = 0u;
	for (uint16_t index = 0u; index < structure_count; index++) {
		if ((size_t)(end - cursor) < 64u) return count;
		uint16_t type, length;
		memcpy(&type, cursor, sizeof(type));
		memcpy(&length, cursor + 2u, sizeof(length));
		if (length < 64u || (size_t)(end - cursor) < length) return count;
		if (type == 0u) {
			uint32_t flags;
			uint64_t address;
			memcpy(&flags, cursor + 4u, sizeof(flags));
			memcpy(&address, cursor + 28u, sizeof(address));
			bool usable = (flags & 1u) == 0u && address != 0u && address <= UINTPTR_MAX;
			if (usable && out_descriptor != NULL && count == target) {
				*out_descriptor = (struct hal_iommu_controller_descriptor){.kind = HAL_IOMMU_KIND_LOONGARCH_V1};
				out_descriptor->firmware_flags = flags;
				memcpy(&out_descriptor->segment, cursor + 8u, sizeof(uint16_t));
				memcpy(&out_descriptor->firmware_physical_address_bits, cursor + 10u, sizeof(uint16_t));
				memcpy(&out_descriptor->firmware_io_address_bits, cursor + 12u, sizeof(uint16_t));
				memcpy(&out_descriptor->firmware_page_table_levels, cursor + 14u, sizeof(uint16_t));
				memcpy(&out_descriptor->firmware_leaf_size_mask, cursor + 16u, sizeof(uint64_t));
				memcpy(&out_descriptor->device_id, cursor + 24u, sizeof(uint32_t));
				memcpy(&out_descriptor->register_size, cursor + 36u, sizeof(uint32_t));
				memcpy(&out_descriptor->maximum_device_count, cursor + 52u, sizeof(uint32_t));
				out_descriptor->register_address = (uintptr_t)address;
			}
			if (usable) count++;
		}
		cursor += length;
	}
	return count;
}

size_t hal_iommu_controller_count(void) {
	return loongarch_iommu_controllers(SIZE_MAX, NULL);
}

bool hal_iommu_controller_at(size_t index, struct hal_iommu_controller_descriptor* out_descriptor) {
	return out_descriptor != NULL && index < loongarch_iommu_controllers(index, out_descriptor);
}

bool hal_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                               const struct hal_iommu_controller_descriptor* descriptor,
                               struct hal_iommu_info*                        out_info) {
	return loongarch_iommu_v1_controller_init(controller, descriptor, out_info);
}

void hal_iommu_controller_deinit(struct hal_iommu_controller_state* controller) {
	loongarch_iommu_v1_controller_deinit(controller);
}

bool hal_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	return loongarch_iommu_v1_mapping_supported(controller, access);
}

bool hal_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                          struct hal_iommu_space_state* space) {
	return loongarch_iommu_v1_space_init(controller, context_id, space);
}

void hal_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	loongarch_iommu_v1_space_deinit(controller, space);
}

bool hal_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request) {
	return loongarch_iommu_v1_map(controller, space, request);
}

bool hal_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     uint64_t io_address, size_t size) {
	return loongarch_iommu_v1_unmap(controller, space, io_address, size);
}

bool hal_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                      uint32_t source_id, struct hal_iommu_attachment_state* attachment) {
	return loongarch_iommu_v1_attach(controller, space, source_id, attachment);
}

bool hal_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                      struct hal_iommu_attachment_state* attachment) {
	return loongarch_iommu_v1_detach(controller, source_id, attachment);
}
