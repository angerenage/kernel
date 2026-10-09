#include "mcfg.h"

#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/acpi.h>
#include <system/capability.h>

#include "../device.h"

#define PCI_ECAM_BUS_SIZE (1ull << 20u)

#define ACPI_MCFG_RESERVED_SIZE 8u
#define ACPI_MCFG_ALLOCATION_SIZE 16u

/* MCFG describes an ACPI ECAM configuration window, not an AML PCI host bridge. */
#define PCI_COMPATIBLE_ECAM "acpi:pci_ecam"

#define PCI_PROPERTY_REGISTER_ADDRESS "register_address"
#define PCI_PROPERTY_REGISTER_SIZE "register_size"
#define PCI_PROPERTY_CONFIG_ACCESS "config_access"
#define PCI_PROPERTY_DOMAIN "domain"
#define PCI_PROPERTY_START_BUS "start_bus"
#define PCI_PROPERTY_END_BUS "end_bus"

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

static syscall_status_t submit_mcfg_allocation(cap_id_t root_cap, const struct acpi_mcfg_allocation* allocation) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	syscall_status_t      status;

	status = device_builder_begin_root(root_cap, &builder);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_compatible(&builder, PCI_COMPATIBLE_ECAM);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(&builder, PCI_PROPERTY_REGISTER_ADDRESS, allocation->register_address);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(&builder, PCI_PROPERTY_REGISTER_SIZE, allocation->register_size);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_string(&builder, PCI_PROPERTY_CONFIG_ACCESS, "ecam");
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(&builder, PCI_PROPERTY_DOMAIN, allocation->segment_group);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_add_u64(&builder, PCI_PROPERTY_START_BUS, allocation->start_bus);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_u64(&builder, PCI_PROPERTY_END_BUS, allocation->end_bus);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status;
}

static syscall_status_t parse_mcfg_table(cap_id_t table_cap, cap_id_t root_cap, size_t* device_count) {
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
		if (*device_count == SIZE_MAX) return SYSCALL_STATUS_FAILED;
		status = submit_mcfg_allocation(root_cap, &allocation);
		if (status != SYSCALL_STATUS_OK) return status;
		(*device_count)++;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_mcfg_parse(cap_id_t provider_cap, cap_id_t root_cap, size_t* device_count) {
	uint64_t         table_count;
	syscall_status_t status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID || device_count == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = acpi_table_count(provider_cap, "MCFG", &table_count);
	if (status != SYSCALL_STATUS_OK) return status;
	for (uint64_t index = 0u; index < table_count; index++) {
		cap_id_t table_cap = CAP_ID_INVALID;

		status = acpi_table_claim(provider_cap, "MCFG", index, &table_cap);
		if (status == SYSCALL_STATUS_OK) status = parse_mcfg_table(table_cap, root_cap, device_count);
		if (table_cap != CAP_ID_INVALID) {
			syscall_status_t drop_status = cap_drop(table_cap);
			if (status == SYSCALL_STATUS_OK) status = drop_status;
		}
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}
