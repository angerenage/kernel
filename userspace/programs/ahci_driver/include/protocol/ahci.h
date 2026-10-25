#pragma once

/* Controller-independent ATA transport exposed by an AHCI port. */

#include <base/cap.h>
#include <stdint.h>

#define AHCI_TRANSPORT_RESOURCE "transport"
#define AHCI_COMPATIBLE_DEVICE "ahci:device"
#define AHCI_COMPATIBLE_SATA "ahci:sata"
#define AHCI_COMPATIBLE_SATAPI "ahci:satapi"

#define AHCI_TRANSPORT_DRIVER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))
#define AHCI_TRANSPORT_MAX_TRANSFER (4u * 1024u * 1024u)

enum ahci_transport_op {
	AHCI_TRANSPORT_OP_OPEN = 0u,
	AHCI_TRANSPORT_OP_ALLOCATE_BUFFER,
	AHCI_TRANSPORT_OP_EXECUTE,
	AHCI_TRANSPORT_OP_RELEASE_BUFFER,
};

enum ahci_device_kind {
	AHCI_DEVICE_SATA = 0u,
	AHCI_DEVICE_SATAPI,
};

enum ahci_data_direction {
	AHCI_DATA_NONE = 0u,
	AHCI_DATA_FROM_DEVICE,
	AHCI_DATA_TO_DEVICE,
};

struct ahci_transport_request_header {
	enum ahci_transport_op op;
};

/* Open an isolated client session on a port transport. */
struct ahci_transport_open_request {
	struct ahci_transport_request_header header;
};

struct ahci_transport_open_response {
	cap_id_t              session_cap;
	enum ahci_device_kind kind;
	uint32_t              port;
	uint32_t              max_transfer_size;
	uint32_t              reserved;
};

/* Allocate DMA-capable storage shared with the transport client. */
struct ahci_transport_allocate_buffer_request {
	struct ahci_transport_request_header header;
	uint32_t                             size;
	uint32_t                             reserved;
};

struct ahci_transport_allocate_buffer_response {
	uint64_t buffer_id;
	cap_id_t memory_cap;
	uint32_t size;
	uint32_t reserved;
};

/* ATA task-file registers and optional ATAPI command packet. */
struct ahci_transport_execute_request {
	struct ahci_transport_request_header header;
	uint64_t                             buffer_id;
	uint32_t                             buffer_offset;
	uint32_t                             transfer_size;
	enum ahci_data_direction             direction;
	uint8_t                              command;
	uint8_t                              feature_low;
	uint8_t                              feature_high;
	uint8_t                              device;
	uint8_t                              lba[6];
	uint16_t                             count;
	uint8_t                              control;
	uint8_t                              packet_size;
	uint8_t                              packet[16];
	uint8_t                              reserved[2];
};

struct ahci_transport_execute_response {
	uint32_t task_file_data;
	uint32_t interrupt_status;
	uint32_t transferred_size;
	uint32_t reserved;
};

/* Release a buffer previously allocated by this session. */
struct ahci_transport_release_buffer_request {
	struct ahci_transport_request_header header;
	uint64_t                             buffer_id;
};
