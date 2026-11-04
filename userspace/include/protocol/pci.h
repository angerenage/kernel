#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stdint.h>

#define PCI_CONTROL_RESOURCE "pci_control"
#define PCI_CONTROL_COMMAND_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_WRITE))
#define PCI_CONTROL_DRIVER_RIGHTS PCI_CONTROL_COMMAND_RIGHTS

enum pci_control_operation {
	PCI_CONTROL_OP_SET_DMA_ENABLED = 1u,
};

struct pci_control_request_header {
	uint64_t operation;
};

struct pci_control_set_dma_enabled_request {
	struct pci_control_request_header header;
	uint8_t                           enabled;
	uint8_t                           reserved[7];
};

struct pci_control_set_dma_enabled_response {
	uint16_t command;
};
