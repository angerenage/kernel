#include <hal/iommu.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_acpi.h"
#include "amd.h"
#include "vtd.h"

static size_t table_units(const struct iommu_acpi_header* table, size_t table_prefix, enum hal_iommu_kind kind,
                          size_t target, struct hal_iommu_controller_descriptor* out) {
	if (table == NULL || table->length < table_prefix) return 0u;
	const uint8_t* cursor = (const uint8_t*)table + table_prefix;
	const uint8_t* end    = (const uint8_t*)table + table->length;
	size_t         count  = 0u;
	while ((size_t)(end - cursor) >= 4u) {
		uint16_t length;
		memcpy(&length, cursor + 2u, sizeof(length));
		if (length < 4u || (size_t)(end - cursor) < length) return count;
		bool selected = kind == HAL_IOMMU_KIND_INTEL_VTD
		                    ? cursor[0] == 0u
		                    : cursor[0] == 0x10u || cursor[0] == 0x11u || cursor[0] == 0x40u;
		if (selected && length >= 16u) {
			uint64_t registers;
			memcpy(&registers, cursor + 8u, sizeof(registers));
			if (registers != 0u) {
				if (out != NULL && count == target) {
					*out = (struct hal_iommu_controller_descriptor){
						.kind = kind, .register_address = (uintptr_t)registers, .firmware_flags = cursor[1]};
					if (kind == HAL_IOMMU_KIND_AMD) {
						uint32_t iv_info;
						memcpy(&iv_info, (const uint8_t*)table + 36u, sizeof(iv_info));
						out->firmware_physical_address_bits = (uint8_t)((iv_info >> 8u) & 0x7fu);
						out->firmware_io_address_bits       = (uint8_t)((iv_info >> 15u) & 0x7fu);
					}
				}
				count++;
			}
		}
		cursor += length;
	}
	return count;
}

size_t hal_iommu_controller_count(void) {
	return table_units(iommu_acpi_table("DMAR"), 48u, HAL_IOMMU_KIND_INTEL_VTD, SIZE_MAX, NULL) +
	       table_units(iommu_acpi_table("IVRS"), 48u, HAL_IOMMU_KIND_AMD, SIZE_MAX, NULL);
}

bool hal_iommu_controller_at(size_t index, struct hal_iommu_controller_descriptor* out_descriptor) {
	if (out_descriptor == NULL) return false;
	size_t count = table_units(iommu_acpi_table("DMAR"), 48u, HAL_IOMMU_KIND_INTEL_VTD, index, out_descriptor);
	if (index < count) return true;
	return index - count <
	       table_units(iommu_acpi_table("IVRS"), 48u, HAL_IOMMU_KIND_AMD, index - count, out_descriptor);
}

bool hal_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                               const struct hal_iommu_controller_descriptor* descriptor,
                               struct hal_iommu_info*                        out_info) {
	if (descriptor == NULL) return false;
	switch (descriptor->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_controller_init(controller, descriptor, out_info);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_controller_init(controller, descriptor, out_info);
	default:
		return false;
	}
}

void hal_iommu_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (controller == NULL) return;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		x86_vtd_controller_deinit(controller);
		break;
	case HAL_IOMMU_KIND_AMD:
		x86_amd_iommu_controller_deinit(controller);
		break;
	default:
		break;
	}
}

bool hal_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_mapping_supported(controller, access);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_mapping_supported(controller, access);
	default:
		return false;
	}
}

bool hal_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                          struct hal_iommu_space_state* space) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_space_init(controller, context_id, space);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_space_init(controller, context_id, space);
	default:
		return false;
	}
}

void hal_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	if (controller == NULL) return;
	if (controller->kind == HAL_IOMMU_KIND_INTEL_VTD) x86_vtd_space_deinit(controller, space);
	else if (controller->kind == HAL_IOMMU_KIND_AMD) x86_amd_iommu_space_deinit(controller, space);
}

bool hal_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_map(controller, space, request);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_map(controller, space, request);
	default:
		return false;
	}
}

bool hal_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     uint64_t io_address, size_t size) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_unmap(controller, space, io_address, size);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_unmap(controller, space, io_address, size);
	default:
		return false;
	}
}

bool hal_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                      uint32_t source_id) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_attach(controller, space, source_id);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_attach(controller, space, source_id);
	default:
		return false;
	}
}

bool hal_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return x86_vtd_detach(controller, source_id);
	case HAL_IOMMU_KIND_AMD:
		return x86_amd_iommu_detach(controller, source_id);
	default:
		return false;
	}
}
