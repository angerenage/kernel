#pragma once

#include <stdint.h>

/* Hardware interface used to access PCI configuration space. */
enum pci_config_access {
	PCI_CONFIG_ACCESS_INVALID = 0,
	PCI_CONFIG_ACCESS_ECAM,
	PCI_CONFIG_ACCESS_COUNT,
};

/* Source-neutral description of one PCI controller. */
struct pci_controller {
	uint64_t               register_address;
	uint64_t               register_size;
	enum pci_config_access access;
	uint16_t               segment_group;
	uint8_t                start_bus;
	uint8_t                end_bus;
};
