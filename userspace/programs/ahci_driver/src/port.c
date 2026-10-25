#include "port.h"

#include <base/memory.h>
#include <stdint.h>
#include <stdio.h>
#include <system/time.h>

#include "registers.h"

#define AHCI_ENGINE_TIMEOUT_MS 1000u

static uint32_t address_low(uintptr_t address) {
	return (uint32_t)address;
}

static uint32_t address_high(uintptr_t address) {
	return sizeof(address) > sizeof(uint32_t) ? (uint32_t)(address >> 32u) : 0u;
}

static bool wait_clear(volatile uint32_t* value, uint32_t mask) {
	for (size_t elapsed = 0u; elapsed < AHCI_ENGINE_TIMEOUT_MS; elapsed++) {
		if ((*value & mask) == 0u) return true;
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	return (*value & mask) == 0u;
}

bool ahci_port_stop(struct ahci_port* port) {
	if (port == NULL || port->registers == NULL) return false;
	port->registers->command &= ~AHCI_PORT_CMD_ST;
	if (!wait_clear(&port->registers->command, AHCI_PORT_CMD_CR)) return false;
	port->registers->command &= ~AHCI_PORT_CMD_FRE;
	if (!wait_clear(&port->registers->command, AHCI_PORT_CMD_FR)) return false;
	port->running = false;
	return true;
}

bool ahci_port_initialize(struct ahci_port* port, volatile struct ahci_port_registers* registers, uint32_t index,
                          enum ahci_device_kind kind, bool address_64bit, const struct ahci_dma_context* dma) {
	uintptr_t command_list_address;
	uintptr_t received_fis_address;

	if (port == NULL || registers == NULL || dma == NULL) return false;
	*port = (struct ahci_port){
		.registers     = registers,
		.transport_cap = CAP_ID_INVALID,
		.object_id     = (uint64_t)index + 1u,
		.index         = index,
		.kind          = kind,
		.command_dma   = {.memory_cap         = CAP_ID_INVALID,
		                  .cpu_mapping_cap    = CAP_ID_INVALID,
		                  .device_mapping_cap = CAP_ID_INVALID},
	};
	if (!ahci_port_stop(port) || registers->command_issue != 0u || registers->sata_active != 0u) return false;
	if (!ahci_dma_buffer_allocate(
			dma, AHCI_COMMAND_DMA_SIZE, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, &port->command_dma))
		return false;
	if (!address_64bit && (port->command_dma.device_address > UINT32_MAX ||
	                       port->command_dma.size - 1u > UINT32_MAX - port->command_dma.device_address)) {
		fprintf(stderr, "ahci: port %u command memory exceeds the controller address width\n", index);
		ahci_dma_buffer_release(&port->command_dma);
		return false;
	}
	command_list_address              = port->command_dma.device_address + AHCI_COMMAND_LIST_OFFSET;
	received_fis_address              = port->command_dma.device_address + AHCI_RECEIVED_FIS_OFFSET;
	registers->command_list_base_low  = address_low(command_list_address);
	registers->command_list_base_high = address_high(command_list_address);
	registers->fis_base_low           = address_low(received_fis_address);
	registers->fis_base_high          = address_high(received_fis_address);
	registers->interrupt_enable       = 0u;
	registers->interrupt_status       = UINT32_MAX;
	registers->sata_error             = UINT32_MAX;
	registers->command |= AHCI_PORT_CMD_FRE;
	registers->command |= AHCI_PORT_CMD_ST;
	port->running = true;
	return true;
}

void ahci_port_deinitialize(struct ahci_port* port) {
	if (port == NULL) return;
	if (port->running && !ahci_port_stop(port))
		fprintf(stderr, "ahci: port %u command engine did not stop\n", port->index);
	if (port->registers != NULL) {
		port->registers->command_list_base_low = port->registers->command_list_base_high = 0u;
		port->registers->fis_base_low = port->registers->fis_base_high = 0u;
	}
	ahci_dma_buffer_release(&port->command_dma);
}
