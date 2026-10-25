#include "command.h"

#include <base/dma.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/memory.h>
#include <system/time.h>

#include "dma.h"
#include "port.h"
#include "registers.h"

#define AHCI_FIS_TYPE_REGISTER_H2D 0x27u
#define AHCI_FIS_COMMAND (1u << 7u)
#define AHCI_COMMAND_FIS_DWORDS 5u
#define AHCI_COMMAND_HEADER_ATAPI (1u << 5u)
#define AHCI_COMMAND_HEADER_WRITE (1u << 6u)
#define AHCI_PRDT_BYTE_COUNT_MASK 0x003fffffu
#define AHCI_COMMAND_TIMEOUT_MS 5000u

struct ahci_command_header {
	uint16_t flags;
	uint16_t prdt_length;
	uint32_t prd_byte_count;
	uint32_t command_table_base_low;
	uint32_t command_table_base_high;
	uint32_t reserved[4];
};

struct ahci_register_h2d_fis {
	uint8_t  type;
	uint8_t  flags;
	uint8_t  command;
	uint8_t  feature_low;
	uint8_t  lba[3];
	uint8_t  device;
	uint8_t  lba_high[3];
	uint8_t  feature_high;
	uint16_t count;
	uint8_t  isochronous_command_completion;
	uint8_t  control;
	uint8_t  reserved[4];
};

struct ahci_prdt_entry {
	uint32_t data_base_low;
	uint32_t data_base_high;
	uint32_t reserved;
	uint32_t byte_count;
};

struct ahci_command_table {
	uint8_t                command_fis[64];
	uint8_t                atapi_command[16];
	uint8_t                reserved[48];
	struct ahci_prdt_entry prdt[1];
};

_Static_assert(sizeof(struct ahci_command_header) == 32u, "invalid AHCI command-header layout");
_Static_assert(sizeof(struct ahci_register_h2d_fis) == 20u, "invalid register FIS layout");
_Static_assert(offsetof(struct ahci_command_table, prdt) == 128u, "invalid AHCI command-table layout");

static uint32_t address_low(uintptr_t address) {
	return (uint32_t)address;
}

static uint32_t address_high(uintptr_t address) {
	return sizeof(address) > sizeof(uint32_t) ? (uint32_t)(address >> 32u) : 0u;
}

static bool wait_device_ready(volatile uint32_t* task_file) {
	for (size_t elapsed = 0u; elapsed < 1000u; elapsed++) {
		if ((*task_file & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) == 0u) return true;
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	return (*task_file & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) == 0u;
}

bool ahci_command_execute(struct ahci_port* port, const struct ahci_transport_execute_request* request,
                          struct ahci_dma_buffer* data, struct ahci_transport_execute_response* response) {
	uint8_t*                      bytes;
	struct ahci_command_header*   header;
	struct ahci_command_table*    table;
	struct ahci_register_h2d_fis* fis;
	uintptr_t                     table_address;
	uintptr_t                     data_address = 0u;
	uint16_t                      flags        = AHCI_COMMAND_FIS_DWORDS;

	if (port == NULL || request == NULL || response == NULL || !port->running) return false;
	if (request->transfer_size != 0u) {
		if (data == NULL || request->buffer_offset > data->size ||
		    request->transfer_size > data->size - request->buffer_offset)
			return false;
		data_address = data->device_address + request->buffer_offset;
	}
	bytes         = port->command_dma.cpu_address;
	header        = (struct ahci_command_header*)(bytes + AHCI_COMMAND_LIST_OFFSET);
	table         = (struct ahci_command_table*)(bytes + AHCI_COMMAND_TABLE_OFFSET);
	fis           = (struct ahci_register_h2d_fis*)table->command_fis;
	table_address = port->command_dma.device_address + AHCI_COMMAND_TABLE_OFFSET;
	memset(header, 0, sizeof(*header));
	memset(table, 0, sizeof(*table));
	if (request->packet_size != 0u) {
		flags |= AHCI_COMMAND_HEADER_ATAPI;
		memcpy(table->atapi_command, request->packet, request->packet_size);
	}
	if (request->direction == AHCI_DATA_TO_DEVICE) flags |= AHCI_COMMAND_HEADER_WRITE;
	header->flags                   = flags;
	header->prdt_length             = request->transfer_size == 0u ? 0u : 1u;
	header->command_table_base_low  = address_low(table_address);
	header->command_table_base_high = address_high(table_address);
	if (request->transfer_size != 0u) {
		table->prdt[0].data_base_low  = address_low(data_address);
		table->prdt[0].data_base_high = address_high(data_address);
		table->prdt[0].byte_count     = (request->transfer_size - 1u) & AHCI_PRDT_BYTE_COUNT_MASK;
	}
	fis->type         = AHCI_FIS_TYPE_REGISTER_H2D;
	fis->flags        = AHCI_FIS_COMMAND;
	fis->command      = request->command;
	fis->feature_low  = request->feature_low;
	fis->feature_high = request->feature_high;
	fis->device       = request->device;
	memcpy(fis->lba, request->lba, 3u);
	memcpy(fis->lba_high, request->lba + 3u, 3u);
	fis->count   = request->count;
	fis->control = request->control;

	if (request->direction == AHCI_DATA_TO_DEVICE &&
	    mapping_sync(data->device_mapping_cap, 0u, data->size, DMA_SYNC_FOR_DEVICE) != SYSCALL_STATUS_OK)
		return false;
	if (mapping_sync(port->command_dma.device_mapping_cap, 0u, port->command_dma.size, DMA_SYNC_FOR_DEVICE) !=
	    SYSCALL_STATUS_OK)
		return false;
	port->registers->interrupt_status = UINT32_MAX;
	port->registers->sata_error       = UINT32_MAX;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!wait_device_ready(&port->registers->task_file_data)) return false;
	port->registers->command_issue = 1u;
	for (size_t elapsed = 0u; elapsed < AHCI_COMMAND_TIMEOUT_MS; elapsed++) {
		uint32_t interrupt_status = port->registers->interrupt_status;
		if ((interrupt_status & AHCI_PORT_IS_TFES) != 0u) break;
		if ((port->registers->command_issue & 1u) == 0u) {
			if (mapping_sync(port->command_dma.device_mapping_cap, 0u, port->command_dma.size, DMA_SYNC_FOR_CPU) !=
			    SYSCALL_STATUS_OK)
				return false;
			response->task_file_data   = port->registers->task_file_data;
			response->interrupt_status = interrupt_status;
			response->transferred_size = header->prd_byte_count;
			response->reserved         = 0u;
			if ((response->task_file_data & (AHCI_PORT_TFD_ERR | AHCI_PORT_TFD_DF)) != 0u) return false;
			if (request->direction == AHCI_DATA_FROM_DEVICE &&
			    mapping_sync(data->device_mapping_cap, 0u, data->size, DMA_SYNC_FOR_CPU) != SYSCALL_STATUS_OK)
				return false;
			return true;
		}
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	response->task_file_data   = port->registers->task_file_data;
	response->interrupt_status = port->registers->interrupt_status;
	response->transferred_size = 0u;
	response->reserved         = 0u;
	return false;
}
