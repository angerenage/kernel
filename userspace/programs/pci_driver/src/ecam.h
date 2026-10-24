#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* One Enhanced Configuration Access Mechanism allocation with a single mapped bus window. */
struct pci_ecam {
	cap_id_t          memory_cap;
	cap_id_t          address_space_cap;
	cap_id_t          mapping_cap;
	volatile uint8_t* base;
	size_t            size;
	uint16_t          segment;
	uint8_t           first_bus;
	uint8_t           last_bus;
	uint8_t           mapped_bus;
	bool              bus_mapped;
};

/* Bind a firmware-described ECAM memory object to the generic configuration interface. */
bool pci_ecam_initialize(struct pci_ecam* ecam, cap_id_t memory_cap, cap_id_t address_space_cap, size_t size,
                         uint16_t segment, uint8_t first_bus, uint8_t last_bus, struct pci_config* out_config);

/* Release the currently mapped ECAM bus window. */
void pci_ecam_deinitialize(struct pci_ecam* ecam);

/* Read an 8-bit register through ECAM. */
bool pci_ecam_read8(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t* out_value);
/* Read a 16-bit register through ECAM. */
bool pci_ecam_read16(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t* out_value);
/* Read a 32-bit register through ECAM. */
bool pci_ecam_read32(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t* out_value);
/* Write an 8-bit register through ECAM. */
bool pci_ecam_write8(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t value);
/* Write a 16-bit register through ECAM. */
bool pci_ecam_write16(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t value);
/* Write a 32-bit register through ECAM. */
bool pci_ecam_write32(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t value);
