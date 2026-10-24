#include "ecam.h"

#include <base/memory.h>
#include <stddef.h>
#include <system/capability.h>
#include <system/memory.h>

#define PCI_ECAM_BUS_SIZE (1u << 20u)
#define PCI_ECAM_DEVICE_SIZE (1u << 15u)
#define PCI_ECAM_FUNCTION_SIZE (1u << 12u)
#define PCI_DEVICE_COUNT 32u
#define PCI_FUNCTION_COUNT 8u

static bool map_bus(struct pci_ecam* ecam, uint8_t bus) {
	struct address_space_map_response mapping = {.mapping_cap = CAP_ID_INVALID};
	cap_id_t                          bus_cap = CAP_ID_INVALID;
	size_t                            bus_offset;

	if (ecam->bus_mapped && ecam->mapped_bus == bus) return true;
	if (ecam->mapping_cap != CAP_ID_INVALID) {
		if (mapping_unmap(ecam->mapping_cap) != SYSCALL_STATUS_OK) return false;
		ecam->mapping_cap = CAP_ID_INVALID;
		ecam->base        = NULL;
		ecam->bus_mapped  = false;
	}
	bus_offset = ((size_t)bus - ecam->first_bus) * PCI_ECAM_BUS_SIZE;
	if (memory_slice(ecam->memory_cap, bus_offset, PCI_ECAM_BUS_SIZE, &bus_cap) != SYSCALL_STATUS_OK) return false;
	if (address_space_map(ecam->address_space_cap,
	                      bus_cap,
	                      MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                      0u,
	                      PCI_ECAM_FUNCTION_SIZE,
	                      0u,
	                      0u,
	                      &mapping) != SYSCALL_STATUS_OK) {
		(void)cap_drop(bus_cap);
		return false;
	}
	(void)cap_drop(bus_cap);
	if (mapping.mapping_cap == CAP_ID_INVALID || mapping.address == 0u) {
		if (mapping.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapping.mapping_cap);
		return false;
	}
	ecam->mapping_cap = mapping.mapping_cap;
	ecam->base        = (volatile uint8_t*)mapping.address;
	ecam->mapped_bus  = bus;
	ecam->bus_mapped  = true;
	return true;
}

static volatile uint8_t* register_address(struct pci_ecam* ecam, struct pci_address address, uint16_t offset,
                                          size_t width) {
	size_t function_offset;

	if (ecam == NULL || address.segment != ecam->segment || address.bus < ecam->first_bus ||
	    address.bus > ecam->last_bus || address.device >= PCI_DEVICE_COUNT || address.function >= PCI_FUNCTION_COUNT)
		return NULL;
	function_offset =
		(size_t)address.device * PCI_ECAM_DEVICE_SIZE + (size_t)address.function * PCI_ECAM_FUNCTION_SIZE + offset;
	if (function_offset > PCI_ECAM_BUS_SIZE || width > PCI_ECAM_BUS_SIZE - function_offset ||
	    !map_bus(ecam, address.bus))
		return NULL;
	return ecam->base + function_offset;
}

bool pci_ecam_read8(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t* out_value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(*out_value));
	if (pointer == NULL) return false;
	*out_value = *pointer;
	return true;
}

bool pci_ecam_read16(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t* out_value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(*out_value));
	if (pointer == NULL) return false;
	*out_value = *(volatile uint16_t*)pointer;
	return true;
}

bool pci_ecam_read32(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t* out_value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(*out_value));
	if (pointer == NULL) return false;
	*out_value = *(volatile uint32_t*)pointer;
	return true;
}

bool pci_ecam_write8(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(value));
	if (pointer == NULL) return false;
	*pointer = value;
	return true;
}

bool pci_ecam_write16(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(value));
	if (pointer == NULL) return false;
	*(volatile uint16_t*)pointer = value;
	return true;
}

bool pci_ecam_write32(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(value));
	if (pointer == NULL) return false;
	*(volatile uint32_t*)pointer = value;
	return true;
}

bool pci_ecam_initialize(struct pci_ecam* ecam, cap_id_t memory_cap, cap_id_t address_space_cap, size_t size,
                         uint16_t segment, uint8_t first_bus, uint8_t last_bus, struct pci_config* out_config) {
	size_t bus_count;

	if (ecam == NULL || memory_cap == CAP_ID_INVALID || address_space_cap == CAP_ID_INVALID || out_config == NULL ||
	    first_bus > last_bus)
		return false;
	bus_count = (size_t)last_bus - first_bus + 1u;
	if (bus_count > SIZE_MAX / PCI_ECAM_BUS_SIZE || size != bus_count * PCI_ECAM_BUS_SIZE) return false;
	*ecam = (struct pci_ecam){
		.memory_cap        = memory_cap,
		.address_space_cap = address_space_cap,
		.mapping_cap       = CAP_ID_INVALID,
		.size              = size,
		.segment           = segment,
		.first_bus         = first_bus,
		.last_bus          = last_bus,
	};
#if defined(__x86_64__)
	*out_config = (struct pci_config){.kind = PCI_CONFIG_KIND_ECAM, .backend.ecam = ecam};
#else
	*out_config = (struct pci_config){.ecam = ecam};
#endif
	return true;
}

void pci_ecam_deinitialize(struct pci_ecam* ecam) {
	if (ecam == NULL) return;
	if (ecam->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(ecam->mapping_cap);
	*ecam = (struct pci_ecam){
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
}
