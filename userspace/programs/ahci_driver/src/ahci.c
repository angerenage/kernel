#include "ahci.h"

#include <base/address_space.h>
#include <base/memory.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>
#include <system/time.h>

#include "registers.h"

#define AHCI_PCI_CLASS_MASS_STORAGE 0x01u
#define AHCI_PCI_SUBCLASS_SATA 0x06u
#define AHCI_PCI_INTERFACE_AHCI 0x01u
#define AHCI_PCI_BAR_MEMORY32 2u
#define AHCI_PCI_BAR_MEMORY64 3u
#define AHCI_ABAR_RESOURCE "bar_5"
#define AHCI_MEMORY_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))

struct ahci_mapping {
	cap_id_t                             memory_cap;
	cap_id_t                             address_space_cap;
	cap_id_t                             mapping_cap;
	volatile struct ahci_host_registers* registers;
	size_t                               bar_size;
};

static bool read_property(cap_id_t device_cap, const char* name, uint64_t* out_value) {
	return device_property_read_u64(device_cap, name, out_value) == SYSCALL_STATUS_OK;
}

static bool validate_device(cap_id_t device_cap, uint64_t* out_bar_size, uint64_t* out_mapping_offset) {
	uint64_t class_code;
	uint64_t subclass;
	uint64_t interface;
	uint64_t bar_kind;
	bool     bar_enabled;
	bool     bar_claimed;

	if (!read_property(device_cap, "class_code", &class_code) || !read_property(device_cap, "subclass", &subclass) ||
	    !read_property(device_cap, "programming_interface", &interface) || class_code != AHCI_PCI_CLASS_MASS_STORAGE ||
	    subclass != AHCI_PCI_SUBCLASS_SATA || interface != AHCI_PCI_INTERFACE_AHCI) {
		fprintf(stderr, "ahci: device is not a PCI AHCI controller\n");
		return false;
	}
	if (!read_property(device_cap, "bar_5_kind", &bar_kind) || !read_property(device_cap, "bar_5_size", out_bar_size) ||
	    !read_property(device_cap, "bar_5_mapping_offset", out_mapping_offset) ||
	    device_property_read_bool(device_cap, "bar_5_enabled", &bar_enabled) != SYSCALL_STATUS_OK ||
	    device_property_read_bool(device_cap, "bar_5_claimed", &bar_claimed) != SYSCALL_STATUS_OK ||
	    (bar_kind != AHCI_PCI_BAR_MEMORY32 && bar_kind != AHCI_PCI_BAR_MEMORY64) ||
	    *out_bar_size < AHCI_GENERIC_REGISTERS_SIZE || !bar_enabled || !bar_claimed) {
		fprintf(stderr, "ahci: ABAR is missing, disabled, or unavailable\n");
		return false;
	}
	return true;
}

static void mapping_release(struct ahci_mapping* mapping) {
	if (mapping == NULL) return;
	if (mapping->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapping->mapping_cap);
	if (mapping->address_space_cap != CAP_ID_INVALID) (void)cap_drop(mapping->address_space_cap);
	if (mapping->memory_cap != CAP_ID_INVALID) (void)cap_drop(mapping->memory_cap);
	*mapping = (struct ahci_mapping){
		.memory_cap        = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.mapping_cap       = CAP_ID_INVALID,
	};
}

static bool mapping_initialize(cap_id_t device_cap, uint64_t bar_size, uint64_t mapping_offset,
                               struct ahci_mapping* out_mapping) {
	struct memory_info                memory;
	struct self_info                  self;
	struct address_space_map_response mapped = {.mapping_cap = CAP_ID_INVALID};

	if (out_mapping == NULL || bar_size > SIZE_MAX || mapping_offset > SIZE_MAX ||
	    mapping_offset > UINT64_MAX - bar_size)
		return false;
	*out_mapping = (struct ahci_mapping){
		.memory_cap        = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.mapping_cap       = CAP_ID_INVALID,
	};
	if (device_resource_acquire(device_cap,
	                            AHCI_ABAR_RESOURCE,
	                            sizeof(AHCI_ABAR_RESOURCE) - 1u,
	                            AHCI_MEMORY_RIGHTS,
	                            &out_mapping->memory_cap) != SYSCALL_STATUS_OK ||
	    memory_info(out_mapping->memory_cap, &memory) != SYSCALL_STATUS_OK ||
	    memory.memory_type != MEMORY_TYPE_DEVICE || mapping_offset + bar_size > memory.size ||
	    process_self_info(&self) != SYSCALL_STATUS_OK)
		goto fail;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.address_space_cap == CAP_ID_INVALID) goto fail;
	out_mapping->address_space_cap = self.address_space_cap;
	if (address_space_map(out_mapping->address_space_cap,
	                      out_mapping->memory_cap,
	                      MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                      0u,
	                      0u,
	                      0u,
	                      0u,
	                      &mapped) != SYSCALL_STATUS_OK ||
	    mapped.mapping_cap == CAP_ID_INVALID)
		goto fail;
	out_mapping->mapping_cap = mapped.mapping_cap;
	if (mapped.address == 0u) goto fail;
	out_mapping->registers = (volatile struct ahci_host_registers*)(mapped.address + (uintptr_t)mapping_offset);
	out_mapping->bar_size  = (size_t)bar_size;
	return true;

fail:
	mapping_release(out_mapping);
	return false;
}

static const char* interface_speed(uint32_t capabilities) {
	switch ((capabilities & AHCI_CAP_ISS_MASK) >> AHCI_CAP_ISS_SHIFT) {
	case 1u:
		return "1.5 Gbit/s";
	case 2u:
		return "3 Gbit/s";
	case 3u:
		return "6 Gbit/s";
	default:
		return "unknown";
	}
}

static const char* device_signature(uint32_t signature) {
	switch (signature) {
	case AHCI_PORT_SIGNATURE_ATA:
		return "SATA";
	case AHCI_PORT_SIGNATURE_ATAPI:
		return "SATAPI";
	case AHCI_PORT_SIGNATURE_SEMB:
		return "SEMB";
	case AHCI_PORT_SIGNATURE_MULTIPLIER:
		return "port multiplier";
	default:
		return "unknown";
	}
}

static const char* link_state(uint32_t status) {
	uint32_t detection = status & AHCI_PORT_SSTS_DET_MASK;
	uint32_t power     = (status & AHCI_PORT_SSTS_IPM_MASK) >> AHCI_PORT_SSTS_IPM_SHIFT;

	if (detection == 0u) return "empty";
	if (detection != 3u) return detection == 1u ? "present, no link" : "offline";
	switch (power) {
	case 1u:
		return "active";
	case 2u:
		return "partial";
	case 6u:
		return "slumber";
	case 8u:
		return "device sleep";
	default:
		return "link established";
	}
}

static bool inspect_ports(const struct ahci_mapping* mapping, uint32_t capabilities, uint32_t implemented) {
	uint32_t port_limit = (capabilities & AHCI_CAP_NP_MASK) + 1u;

	if (port_limit > AHCI_PORT_COUNT || (port_limit < AHCI_PORT_COUNT && (implemented >> port_limit) != 0u)) {
		fprintf(stderr, "ahci: invalid implemented-port bitmap\n");
		return false;
	}
	for (uint32_t index = 0u; index < port_limit; index++) {
		size_t                               offset;
		volatile struct ahci_port_registers* port;
		uint32_t                             status;
		uint32_t                             signature;

		if ((implemented & (1u << index)) == 0u) continue;
		offset = AHCI_PORT_REGISTERS_OFFSET + (size_t)index * AHCI_PORT_REGISTERS_SIZE;
		if (offset > mapping->bar_size || sizeof(*port) > mapping->bar_size - offset) {
			fprintf(stderr, "ahci: ABAR does not contain implemented port %u\n", index);
			return false;
		}
		port      = (volatile struct ahci_port_registers*)((volatile uint8_t*)mapping->registers + offset);
		status    = port->sata_status;
		signature = port->signature;
		printf("ahci: port %u link=%s signature=%08x (%s) task_file=%08x\n",
		       index,
		       link_state(status),
		       signature,
		       device_signature(signature),
		       port->task_file_data);
	}
	return true;
}

bool ahci_driver_run(cap_id_t device_cap) {
	struct ahci_mapping mapping;
	uint64_t            bar_size;
	uint64_t            mapping_offset;
	uint32_t            capabilities;
	uint32_t            version;
	uint32_t            implemented;
	uint32_t            control;
	bool                success;

	if (device_cap == CAP_ID_INVALID || !validate_device(device_cap, &bar_size, &mapping_offset)) return false;
	if (!mapping_initialize(device_cap, bar_size, mapping_offset, &mapping)) {
		fprintf(stderr, "ahci: unable to map ABAR\n");
		return false;
	}
	capabilities = mapping.registers->capabilities;
	control      = mapping.registers->global_host_control;
	implemented  = mapping.registers->ports_implemented;
	version      = mapping.registers->version;
	printf("ahci: version %x.%02x, %u port(s), %u command slot(s), speed=%s, 64-bit=%s, ncq=%s, mode=%s\n",
	       version >> 16u,
	       version & 0xffffu,
	       (capabilities & AHCI_CAP_NP_MASK) + 1u,
	       ((capabilities & AHCI_CAP_NCS_MASK) >> AHCI_CAP_NCS_SHIFT) + 1u,
	       interface_speed(capabilities),
	       (capabilities & AHCI_CAP_S64A) != 0u ? "yes" : "no",
	       (capabilities & AHCI_CAP_SNCQ) != 0u ? "yes" : "no",
	       (control & AHCI_GHC_AE) != 0u ? "AHCI" : "legacy");
	success = inspect_ports(&mapping, capabilities, implemented);
	while (success) success = sleep_ms(1000u) == SYSCALL_STATUS_OK;
	mapping_release(&mapping);
	return success;
}
