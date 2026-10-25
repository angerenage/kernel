#pragma once

#include <base/cap.h>
#include <protocol/ahci.h>
#include <stdbool.h>
#include <stdint.h>

#include "dma.h"

#define AHCI_COMMAND_LIST_OFFSET 0u
#define AHCI_COMMAND_LIST_SIZE 1024u
#define AHCI_RECEIVED_FIS_OFFSET (AHCI_COMMAND_LIST_OFFSET + AHCI_COMMAND_LIST_SIZE)
#define AHCI_RECEIVED_FIS_SIZE 256u
#define AHCI_COMMAND_TABLE_OFFSET (AHCI_RECEIVED_FIS_OFFSET + AHCI_RECEIVED_FIS_SIZE)
#define AHCI_COMMAND_DMA_SIZE 1536u

struct ahci_port_registers;

/* One initialized physical AHCI port and its private command memory. */
struct ahci_port {
	volatile struct ahci_port_registers* registers;
	struct ahci_dma_buffer               command_dma;
	cap_id_t                             transport_cap;
	uint64_t                             object_id;
	uint32_t                             index;
	enum ahci_device_kind                kind;
	bool                                 running;
	bool                                 published;
};

/* Stop a port command engine and wait until its state is stable. */
bool ahci_port_stop(struct ahci_port* port);

/* Allocate command state and start a populated ATA or ATAPI port. */
bool ahci_port_initialize(struct ahci_port* port, volatile struct ahci_port_registers* registers, uint32_t index,
                          enum ahci_device_kind kind, bool address_64bit, const struct ahci_dma_context* dma);

/* Stop a port and release its private command state. */
void ahci_port_deinitialize(struct ahci_port* port);
