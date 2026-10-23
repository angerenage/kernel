#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* One mapped Enhanced Configuration Access Mechanism allocation. */
struct pci_ecam {
	volatile uint8_t* base;
	size_t            size;
	uint16_t          segment;
	uint8_t           first_bus;
	uint8_t           last_bus;
};

/* Bind an already mapped firmware-described ECAM window to the generic interface. */
bool pci_ecam_initialize(struct pci_ecam* ecam, volatile void* base, size_t size, uint16_t segment, uint8_t first_bus,
                         uint8_t last_bus, struct pci_config* out_config);

/* Read an 8-bit register through ECAM. */
bool pci_ecam_read8(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t* out_value);
/* Read a 16-bit register through ECAM. */
bool pci_ecam_read16(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t* out_value);
/* Read a 32-bit register through ECAM. */
bool pci_ecam_read32(const struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t* out_value);
/* Write an 8-bit register through ECAM. */
bool pci_ecam_write8(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint8_t value);
/* Write a 16-bit register through ECAM. */
bool pci_ecam_write16(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint16_t value);
/* Write a 32-bit register through ECAM. */
bool pci_ecam_write32(struct pci_ecam* ecam, struct pci_address address, uint16_t offset, uint32_t value);
