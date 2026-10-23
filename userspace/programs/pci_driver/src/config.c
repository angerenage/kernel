#include "config.h"

#include <stddef.h>

#include "ecam.h"

#define PCI_CONFIG_SPACE_SIZE 4096u

static bool access_valid(const struct pci_config* config, uint16_t offset, uint16_t width, uint16_t alignment) {
	if (config == NULL || offset >= PCI_CONFIG_SPACE_SIZE || width > PCI_CONFIG_SPACE_SIZE - offset ||
	    (offset & (alignment - 1u)) != 0u)
		return false;
#if defined(__x86_64__)
	return config->kind != PCI_CONFIG_KIND_INVALID;
#else
	return config->ecam != NULL;
#endif
}

bool pci_config_read8(const struct pci_config* config, struct pci_address address, uint16_t offset,
                      uint8_t* out_value) {
	if (out_value == NULL || !access_valid(config, offset, sizeof(*out_value), sizeof(*out_value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_read8(config->backend.ecam, address, offset, out_value);
	default:
		return false;
	}
#else
	return pci_ecam_read8(config->ecam, address, offset, out_value);
#endif
}

bool pci_config_read16(const struct pci_config* config, struct pci_address address, uint16_t offset,
                       uint16_t* out_value) {
	if (out_value == NULL || !access_valid(config, offset, sizeof(*out_value), sizeof(*out_value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_read16(config->backend.ecam, address, offset, out_value);
	default:
		return false;
	}
#else
	return pci_ecam_read16(config->ecam, address, offset, out_value);
#endif
}

bool pci_config_read32(const struct pci_config* config, struct pci_address address, uint16_t offset,
                       uint32_t* out_value) {
	if (out_value == NULL || !access_valid(config, offset, sizeof(*out_value), sizeof(*out_value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_read32(config->backend.ecam, address, offset, out_value);
	default:
		return false;
	}
#else
	return pci_ecam_read32(config->ecam, address, offset, out_value);
#endif
}

bool pci_config_write8(struct pci_config* config, struct pci_address address, uint16_t offset, uint8_t value) {
	if (!access_valid(config, offset, sizeof(value), sizeof(value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_write8(config->backend.ecam, address, offset, value);
	default:
		return false;
	}
#else
	return pci_ecam_write8(config->ecam, address, offset, value);
#endif
}

bool pci_config_write16(struct pci_config* config, struct pci_address address, uint16_t offset, uint16_t value) {
	if (!access_valid(config, offset, sizeof(value), sizeof(value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_write16(config->backend.ecam, address, offset, value);
	default:
		return false;
	}
#else
	return pci_ecam_write16(config->ecam, address, offset, value);
#endif
}

bool pci_config_write32(struct pci_config* config, struct pci_address address, uint16_t offset, uint32_t value) {
	if (!access_valid(config, offset, sizeof(value), sizeof(value))) return false;
#if defined(__x86_64__)
	switch (config->kind) {
	case PCI_CONFIG_KIND_ECAM:
		return pci_ecam_write32(config->backend.ecam, address, offset, value);
	default:
		return false;
	}
#else
	return pci_ecam_write32(config->ecam, address, offset, value);
#endif
}
