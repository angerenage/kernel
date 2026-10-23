#pragma once

#include <base/cap.h>
#include <base/interrupt.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define PCI_RESOURCE_BAR_COUNT 6u

/* Kind of address range implemented by a PCI BAR. */
enum pci_bar_kind {
	PCI_BAR_NONE = 0u,
	PCI_BAR_IO,
	PCI_BAR_MEMORY32,
	PCI_BAR_MEMORY64,
	PCI_BAR_RESERVED,
};

/* One decoded PCI base address register. */
struct pci_bar_resource {
	enum pci_bar_kind kind;
	uint8_t           index;
	uint64_t          address; /* PCI bus address; firmware translation may still be required. */
	uint64_t          size;
	bool              prefetchable;
	cap_id_t          capability;
};

/* Expansion ROM address range, when implemented. */
struct pci_rom_resource {
	uint64_t address; /* PCI bus address; firmware translation may still be required. */
	uint64_t size;
	bool     present;
	bool     enabled;
	cap_id_t capability;
};

/* Conventional INTx routing information from configuration space. */
struct pci_intx_resource {
	uint8_t line;
	uint8_t pin;
};

/* Message-signalled interrupt capability. */
struct pci_msi_resource {
	uint8_t                  capability_offset;
	uint16_t                 vector_count;
	bool                     present;
	bool                     enabled;
	bool                     address_64bit;
	bool                     per_vector_masking;
	cap_id_t                 capability;
	struct interrupt_message message;
};

/* MSI-X table and pending-bit array locations. */
struct pci_msix_resource {
	uint8_t  capability_offset;
	uint8_t  table_bar;
	uint8_t  pba_bar;
	uint16_t vector_count;
	uint32_t table_offset;
	uint32_t pba_offset;
	bool     present;
	bool     enabled;
	bool     function_masked;
};

/* One address range forwarded by a PCI-to-PCI bridge. */
struct pci_bridge_window {
	uint64_t base; /* PCI bus address forwarded downstream. */
	uint64_t size;
	bool     present;
	bool     prefetchable;
};

/* Bus numbers and forwarding windows from a type-one header. */
struct pci_bridge_resources {
	uint8_t                  primary_bus;
	uint8_t                  secondary_bus;
	uint8_t                  subordinate_bus;
	struct pci_bridge_window io;
	struct pci_bridge_window memory;
	struct pci_bridge_window prefetchable_memory;
	bool                     present;
};

/* Resource information needed to construct device-manager resources later. */
struct pci_resources {
	uint16_t                    command;
	uint16_t                    status;
	uint16_t                    requester_id;
	uint8_t                     bar_count;
	struct pci_bar_resource     bars[PCI_RESOURCE_BAR_COUNT];
	struct pci_rom_resource     rom;
	struct pci_intx_resource    intx;
	struct pci_msi_resource     msi;
	struct pci_msix_resource    msix;
	struct pci_bridge_resources bridge;
};

/* Probe and restore a function's configuration registers while collecting its resources. */
bool pci_resources_detect(struct pci_config* config, struct pci_address address, uint8_t header_type,
                          struct pci_resources* out_resources);
