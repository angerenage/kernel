#include "host.h"

#include <base/memory.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>
#include <system/trace.h>

#define PCI_ECAM_BUS_SIZE (1u << 20u)
#define PCI_MEMORY_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DERIVE))

#define MCFG_RESOURCE_CONFIGURATION_SPACE "mcfg_configuration_space"
#define MCFG_PROPERTY_SEGMENT_GROUP "mcfg_segment_group"
#define MCFG_PROPERTY_START_BUS "mcfg_start_bus"
#define MCFG_PROPERTY_END_BUS "mcfg_end_bus"
#define DT_PROPERTY_BUS_RANGE "bus_range"
#define DT_PROPERTY_DOMAIN "linux_pci_domain"

TRACE_LABEL(trace_host_description, "pci/host/read_description");
TRACE_LABEL(trace_host_map, "pci/host/map_configuration");

static uint32_t read_u32_be(const uint8_t value[4]) {
	return ((uint32_t)value[0] << 24u) | ((uint32_t)value[1] << 16u) | ((uint32_t)value[2] << 8u) | (uint32_t)value[3];
}

static bool read_dt_u32(cap_id_t device_cap, const char* name, uint32_t* out_value) {
	struct device_property_info info;
	uint8_t                     encoded[4];
	size_t                      name_size = strlen(name);

	if (out_value == NULL || device_property_get_info(device_cap, name, name_size, &info) != SYSCALL_STATUS_OK ||
	    info.type != DEVICE_PROPERTY_BYTES || info.element_count != sizeof(encoded) ||
	    info.value_size != sizeof(encoded) ||
	    device_property_read_bytes(device_cap, name, 0u, encoded, sizeof(encoded)) != SYSCALL_STATUS_OK)
		return false;
	*out_value = read_u32_be(encoded);
	return true;
}

static bool read_dt_bus_range(cap_id_t device_cap, uint8_t* out_first, uint8_t* out_last) {
	struct device_property_info info;
	uint8_t                     encoded[8];
	uint32_t                    first;
	uint32_t                    last;

	if (device_property_get_info(device_cap, DT_PROPERTY_BUS_RANGE, sizeof(DT_PROPERTY_BUS_RANGE) - 1u, &info) !=
	        SYSCALL_STATUS_OK ||
	    info.type != DEVICE_PROPERTY_BYTES || info.element_count != sizeof(encoded) ||
	    info.value_size != sizeof(encoded) ||
	    device_property_read_bytes(device_cap, DT_PROPERTY_BUS_RANGE, 0u, encoded, sizeof(encoded)) !=
	        SYSCALL_STATUS_OK)
		return false;
	first = read_u32_be(encoded);
	last  = read_u32_be(encoded + 4u);
	if (first > UINT8_MAX || last > UINT8_MAX || first > last) return false;
	*out_first = (uint8_t)first;
	*out_last  = (uint8_t)last;
	return true;
}

static bool acquire_configuration(cap_id_t device_cap, cap_id_t* out_cap) {
	static const char* names[] = {
		MCFG_RESOURCE_CONFIGURATION_SPACE,
		"configuration_space",
		"config",
		"ecam",
		"registers",
		"registers_0",
	};

	*out_cap = CAP_ID_INVALID;
	for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++)
		if (device_resource_acquire(device_cap, names[index], strlen(names[index]), PCI_MEMORY_RIGHTS, out_cap) ==
		    SYSCALL_STATUS_OK)
			return true;
	fprintf(stderr, "pci: configuration-space resource is unavailable\n");
	return false;
}

static bool read_description(cap_id_t device_cap, struct pci_host* host) {
	struct memory_info memory;
	uint64_t           segment;
	uint64_t           first;
	uint64_t           last;
	uint32_t           dt_domain;
	size_t             bus_count;
	syscall_status_t   segment_status;
	syscall_status_t   first_status;
	syscall_status_t   last_status;

	if (!acquire_configuration(device_cap, &host->memory_cap)) return false;
	if (memory_info(host->memory_cap, &memory) != SYSCALL_STATUS_OK || memory.memory_type != MEMORY_TYPE_DEVICE ||
	    memory.size == 0u || memory.size % PCI_ECAM_BUS_SIZE != 0u || memory.size / PCI_ECAM_BUS_SIZE > 256u) {
		fprintf(stderr, "pci: invalid ECAM memory capability\n");
		return false;
	}
	segment_status = device_property_read_u64(device_cap, MCFG_PROPERTY_SEGMENT_GROUP, &segment);
	first_status   = device_property_read_u64(device_cap, MCFG_PROPERTY_START_BUS, &first);
	last_status    = device_property_read_u64(device_cap, MCFG_PROPERTY_END_BUS, &last);
	if (segment_status == SYSCALL_STATUS_OK || first_status == SYSCALL_STATUS_OK || last_status == SYSCALL_STATUS_OK) {
		if (segment_status != SYSCALL_STATUS_OK || first_status != SYSCALL_STATUS_OK ||
		    last_status != SYSCALL_STATUS_OK)
			return false;
		if (segment > UINT16_MAX || first > UINT8_MAX || last > UINT8_MAX || first > last) return false;
		host->segment   = (uint16_t)segment;
		host->first_bus = (uint8_t)first;
		host->last_bus  = (uint8_t)last;
		pci_host_ranges_initialize_identity(&host->ranges);
	}
	else {
		host->segment = read_dt_u32(device_cap, DT_PROPERTY_DOMAIN, &dt_domain) && dt_domain <= UINT16_MAX
		                    ? (uint16_t)dt_domain
		                    : 0u;
		if (!read_dt_bus_range(device_cap, &host->first_bus, &host->last_bus)) {
			host->first_bus = 0u;
			host->last_bus  = (uint8_t)(memory.size / PCI_ECAM_BUS_SIZE - 1u);
		}
		if (!pci_host_ranges_read_dt(device_cap, &host->ranges)) {
			fprintf(stderr, "pci: invalid or missing Device Tree host ranges\n");
			return false;
		}
	}
	bus_count = (size_t)host->last_bus - host->first_bus + 1u;
	if (bus_count > SIZE_MAX / PCI_ECAM_BUS_SIZE || memory.size != bus_count * PCI_ECAM_BUS_SIZE) {
		fprintf(stderr,
		        "pci: ECAM size %zu does not match bus range %u-%u\n",
		        memory.size,
		        host->first_bus,
		        host->last_bus);
		return false;
	}
	host->configuration_size = memory.size;
	return true;
}

static bool map_configuration(struct pci_host* host) {
	struct self_info self;

	if (process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.address_space_cap == CAP_ID_INVALID) return false;
	host->address_space_cap = self.address_space_cap;
	return pci_ecam_initialize(&host->ecam,
	                           host->memory_cap,
	                           host->address_space_cap,
	                           host->configuration_size,
	                           host->segment,
	                           host->first_bus,
	                           host->last_bus,
	                           &host->config);
}

bool pci_host_initialize(cap_id_t device_cap, struct pci_host* out_host) {
	bool success;

	if (out_host == NULL) return false;
	*out_host = (struct pci_host){.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID};
	if (device_cap == CAP_ID_INVALID) return false;
	{
		TRACE_SCOPE(trace, &trace_host_description);
		success = read_description(device_cap, out_host);
	}
	if (!success) return false;
	{
		TRACE_SCOPE(trace, &trace_host_map);
		success = map_configuration(out_host);
	}
	return success;
}

void pci_host_deinitialize(struct pci_host* host) {
	if (host == NULL) return;
	pci_ecam_deinitialize(&host->ecam);
	if (host->address_space_cap != CAP_ID_INVALID) (void)cap_drop(host->address_space_cap);
	if (host->memory_cap != CAP_ID_INVALID) (void)cap_drop(host->memory_cap);
	*host = (struct pci_host){.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID};
}
