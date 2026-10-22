#include "mcfg.h"

#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/acpi.h>
#include <system/capability.h>

#include "../aml.h"
#include "../device.h"

#define PCI_ECAM_BUS_SIZE (1ull << 20u)

#define ACPI_MCFG_RESERVED_SIZE 8u
#define ACPI_MCFG_ALLOCATION_SIZE 16u

#define MCFG_COMPATIBLE "acpi:MCFG"
#define PCI_COMPATIBLE_ROOT "acpi:PNP0A03"
#define PCI_COMPATIBLE_EXPRESS_ROOT "acpi:PNP0A08"

#define MCFG_PROPERTY_SEGMENT_GROUP "mcfg_segment_group"
#define MCFG_PROPERTY_START_BUS "mcfg_start_bus"
#define MCFG_PROPERTY_END_BUS "mcfg_end_bus"
#define MCFG_RESOURCE_CONFIGURATION_SPACE "mcfg_configuration_space"

struct acpi_mcfg_allocation {
	uint64_t register_address;
	uint64_t register_size;
	uint16_t segment_group;
	uint8_t  start_bus;
	uint8_t  end_bus;
};

static uint16_t read_u16_le(const uint8_t value[2]) {
	return (uint16_t)value[0] | ((uint16_t)value[1] << 8u);
}

static uint32_t read_u32_le(const uint8_t value[4]) {
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8u) | ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
}

static uint64_t read_u64_le(const uint8_t value[8]) {
	uint64_t result = 0u;

	for (size_t index = 0u; index < 8u; index++) result |= (uint64_t)value[index] << (index * 8u);
	return result;
}

static bool mcfg_allocation_decode(const uint8_t                bytes[ACPI_MCFG_ALLOCATION_SIZE],
                                   struct acpi_mcfg_allocation* out_allocation) {
	uint64_t address;
	uint64_t bus_offset;
	uint64_t bus_count;
	uint64_t range_size;
	uint16_t segment_group;
	uint8_t  start_bus;
	uint8_t  end_bus;

	if (bytes == NULL || out_allocation == NULL) return false;
	address       = read_u64_le(bytes);
	segment_group = read_u16_le(bytes + 8u);
	start_bus     = bytes[10];
	end_bus       = bytes[11];
	if (address == 0u || (address & (PCI_ECAM_BUS_SIZE - 1u)) != 0u || start_bus > end_bus ||
	    read_u32_le(bytes + 12u) != 0u)
		return false;
	bus_offset = (uint64_t)start_bus * PCI_ECAM_BUS_SIZE;
	bus_count  = (uint64_t)end_bus - start_bus + 1u;
	range_size = bus_count * PCI_ECAM_BUS_SIZE;
	if (address > UINT64_MAX - bus_offset || address + bus_offset > UINT64_MAX - range_size) return false;
	*out_allocation = (struct acpi_mcfg_allocation){
		.register_address = address + bus_offset,
		.register_size    = range_size,
		.segment_group    = segment_group,
		.start_bus        = start_bus,
		.end_bus          = end_bus,
	};
	return true;
}

static struct acpi_device* find_pci_host(struct acpi_device_set*            devices,
                                         const struct acpi_mcfg_allocation* allocation) {
	for (struct acpi_device* device = acpi_device_first(devices); device != NULL; device = acpi_device_next(device)) {
		uint64_t         segment  = 0u;
		uint64_t         base_bus = 0u;
		syscall_status_t status;

		if (!acpi_device_has_compatible(device, PCI_COMPATIBLE_ROOT) &&
		    !acpi_device_has_compatible(device, PCI_COMPATIBLE_EXPRESS_ROOT))
			continue;
		status = acpi_aml_evaluate_u64(device, "_SEG", &segment);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE) continue;
		status = acpi_aml_evaluate_u64(device, "_BBN", &base_bus);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE) continue;
		if (segment > UINT16_MAX || base_bus > UINT8_MAX) continue;
		if (segment == allocation->segment_group && base_bus == allocation->start_bus) return device;
	}
	return NULL;
}

static syscall_status_t submit_mcfg_allocation(struct acpi_device_set* devices, cap_id_t memory_allocator_cap,
                                               const struct acpi_mcfg_allocation* allocation) {
	struct acpi_device*    device = find_pci_host(devices, allocation);
	struct device_builder* builder;
	syscall_status_t       status;

	if (device == NULL) {
		status = acpi_device_begin(devices, &device);
		if (status != SYSCALL_STATUS_OK) return status;
		status = acpi_device_add_compatible(device, MCFG_COMPATIBLE);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	builder = acpi_device_builder(device);
	status  = device_builder_add_mmio_resource(builder,
	                                           memory_allocator_cap,
	                                           MCFG_RESOURCE_CONFIGURATION_SPACE,
	                                           sizeof(MCFG_RESOURCE_CONFIGURATION_SPACE) - 1u,
	                                           (uintptr_t)allocation->register_address,
	                                           (size_t)allocation->register_size);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(builder, MCFG_PROPERTY_SEGMENT_GROUP, allocation->segment_group);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(builder, MCFG_PROPERTY_START_BUS, allocation->start_bus);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_u64(builder, MCFG_PROPERTY_END_BUS, allocation->end_bus);
	return status;
}

static syscall_status_t parse_mcfg_table(cap_id_t table_cap, struct acpi_device_set* devices,
                                         cap_id_t memory_allocator_cap) {
	struct acpi_table_info_response info;
	uint8_t                         reserved[ACPI_MCFG_RESERVED_SIZE];
	uint64_t                        allocation_count;
	syscall_status_t                status;

	status = acpi_table_info(table_cap, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.body_size < ACPI_MCFG_RESERVED_SIZE ||
	    (info.body_size - ACPI_MCFG_RESERVED_SIZE) % ACPI_MCFG_ALLOCATION_SIZE != 0u)
		return SYSCALL_STATUS_OK;
	status = acpi_table_read(table_cap, 0u, reserved, sizeof(reserved));
	if (status != SYSCALL_STATUS_OK) return status;
	for (size_t index = 0u; index < sizeof(reserved); index++)
		if (reserved[index] != 0u) return SYSCALL_STATUS_OK;
	allocation_count = (info.body_size - ACPI_MCFG_RESERVED_SIZE) / ACPI_MCFG_ALLOCATION_SIZE;

	for (uint64_t index = 0u; index < allocation_count; index++) {
		uint8_t                     bytes[ACPI_MCFG_ALLOCATION_SIZE];
		struct acpi_mcfg_allocation allocation;
		uint64_t                    offset = ACPI_MCFG_RESERVED_SIZE + index * ACPI_MCFG_ALLOCATION_SIZE;

		status = acpi_table_read(table_cap, offset, bytes, sizeof(bytes));
		if (status != SYSCALL_STATUS_OK) return status;
		if (!mcfg_allocation_decode(bytes, &allocation)) continue;
		status = submit_mcfg_allocation(devices, memory_allocator_cap, &allocation);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_mcfg_parse(cap_id_t provider_cap, struct acpi_device_set* devices,
                                 cap_id_t memory_allocator_cap) {
	uint64_t         table_count;
	syscall_status_t status;

	if (provider_cap == CAP_ID_INVALID || devices == NULL || memory_allocator_cap == CAP_ID_INVALID)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = acpi_table_count(provider_cap, "MCFG", &table_count);
	if (status != SYSCALL_STATUS_OK) return status;
	for (uint64_t index = 0u; index < table_count; index++) {
		cap_id_t table_cap = CAP_ID_INVALID;

		status = acpi_table_claim(provider_cap, "MCFG", index, &table_cap);
		if (status == SYSCALL_STATUS_OK) status = parse_mcfg_table(table_cap, devices, memory_allocator_cap);
		if (table_cap != CAP_ID_INVALID) {
			syscall_status_t drop_status = cap_drop(table_cap);
			if (status == SYSCALL_STATUS_OK) status = drop_status;
		}
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}
