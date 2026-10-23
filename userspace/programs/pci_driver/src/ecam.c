#include "ecam.h"

#include <stddef.h>

#define PCI_ECAM_BUS_SIZE (1u << 20u)
#define PCI_ECAM_DEVICE_SIZE (1u << 15u)
#define PCI_ECAM_FUNCTION_SIZE (1u << 12u)
#define PCI_DEVICE_COUNT 32u
#define PCI_FUNCTION_COUNT 8u

static volatile uint8_t* register_address(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset,
                                          size_t width) {
	size_t bus_offset;
	size_t function_offset;

	if (ecam == NULL || ecam->base == NULL || address.segment != ecam->segment || address.bus < ecam->first_bus ||
	    address.bus > ecam->last_bus || address.device >= PCI_DEVICE_COUNT || address.function >= PCI_FUNCTION_COUNT)
		return NULL;
	bus_offset = ((size_t)address.bus - ecam->first_bus) * PCI_ECAM_BUS_SIZE;
	function_offset =
		(size_t)address.device * PCI_ECAM_DEVICE_SIZE + (size_t)address.function * PCI_ECAM_FUNCTION_SIZE + offset;
	if (bus_offset > ecam->size || function_offset > ecam->size - bus_offset ||
	    width > ecam->size - bus_offset - function_offset)
		return NULL;
	return ecam->base + bus_offset + function_offset;
}

bool pci_ecam_read8(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t* out_value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(*out_value));
	if (pointer == NULL) return false;
	*out_value = *pointer;
	return true;
}

bool pci_ecam_read16(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t* out_value) {
	volatile uint8_t* pointer = register_address(ecam, address, offset, sizeof(*out_value));
	if (pointer == NULL) return false;
	*out_value = *(volatile uint16_t*)pointer;
	return true;
}

bool pci_ecam_read32(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t* out_value) {
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

bool pci_ecam_initialize(struct pci_ecam* ecam, volatile void* base, size_t size, uint16_t segment, uint8_t first_bus,
                         uint8_t last_bus, struct pci_config* out_config) {
	size_t bus_count;

	if (ecam == NULL || base == NULL || out_config == NULL || first_bus > last_bus) return false;
	bus_count = (size_t)last_bus - first_bus + 1u;
	if (bus_count > SIZE_MAX / PCI_ECAM_BUS_SIZE || size != bus_count * PCI_ECAM_BUS_SIZE) return false;
	*ecam = (struct pci_ecam){
		.base      = base,
		.size      = size,
		.segment   = segment,
		.first_bus = first_bus,
		.last_bus  = last_bus,
	};
#if defined(__x86_64__)
	*out_config = (struct pci_config){.kind = PCI_CONFIG_KIND_ECAM, .backend.ecam = ecam};
#else
	*out_config = (struct pci_config){.ecam = ecam};
#endif
	return true;
}
