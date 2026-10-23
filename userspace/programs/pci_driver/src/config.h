#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Transport-independent address of one PCI function. */
struct pci_address {
	uint16_t segment;
	uint8_t  bus;
	uint8_t  device;
	uint8_t  function;
};

struct pci_ecam;
#if defined(__x86_64__)
struct pci_legacy_config; /* Not implemented yet */

/* Configuration transports selectable on x86_64; legacy access is reserved for its future backend. */
enum pci_config_kind {
	PCI_CONFIG_KIND_INVALID = 0u,
	PCI_CONFIG_KIND_ECAM,
	PCI_CONFIG_KIND_LEGACY,
};
#endif

/* Selected configuration backend; non-x86 targets store only their ECAM context. */
struct pci_config {
#if defined(__x86_64__)
	enum pci_config_kind kind;
	union {
		struct pci_ecam*          ecam;
		struct pci_legacy_config* legacy;
	} backend;
#else
	struct pci_ecam* ecam;
#endif
};

/* Read an 8-bit configuration register. */
bool pci_config_read8(const struct pci_config* config, struct pci_address address, uint16_t offset, uint8_t* out_value);

/* Read an aligned 16-bit configuration register. */
bool pci_config_read16(const struct pci_config* config, struct pci_address address, uint16_t offset,
                       uint16_t* out_value);
/* Read an aligned 32-bit configuration register. */
bool pci_config_read32(const struct pci_config* config, struct pci_address address, uint16_t offset,
                       uint32_t* out_value);

/* Write an 8-bit configuration register. */
bool pci_config_write8(struct pci_config* config, struct pci_address address, uint16_t offset, uint8_t value);

/* Write an aligned 16-bit configuration register. */
bool pci_config_write16(struct pci_config* config, struct pci_address address, uint16_t offset, uint16_t value);

/* Write an aligned 32-bit configuration register. */
bool pci_config_write32(struct pci_config* config, struct pci_address address, uint16_t offset, uint32_t value);
