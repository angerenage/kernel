#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/cap.h>

#include <protocol/ahci.h>

/* One client mapping of storage allocated and DMA-mapped by AHCI. */
struct ata_buffer {
	uint64_t id;
	cap_id_t memory_cap;
	cap_id_t address_space_cap;
	cap_id_t mapping_cap;
	uint8_t* address;
	uint32_t size;
};

/* One exclusive session with an AHCI ATA-family port. */
struct ata_transport {
	cap_id_t root_cap;
	cap_id_t session_cap;
	enum ahci_device_kind kind;
	uint32_t port;
	uint32_t max_transfer_size;
};

/* Acquire the child resource and open a SATA or SATAPI transport session. */
bool ata_transport_initialize(
		cap_id_t device_cap, enum ahci_device_kind expected_kind, struct ata_transport* out_transport);

/* Allocate and map a buffer suitable for commands on this transport. */
bool ata_buffer_allocate(const struct ata_transport* transport, uint32_t size, struct ata_buffer* out_buffer);

/* Execute one synchronous ATA-family command through AHCI. */
bool ata_transport_execute(const struct ata_transport* transport, const struct ahci_transport_execute_request* request,
		struct ahci_transport_execute_response* out_response);

/* Unmap and release an AHCI-owned DMA buffer. */
void ata_buffer_release(const struct ata_transport* transport, struct ata_buffer* buffer);

/* Close a transport session after all buffers have been released. */
void ata_transport_deinitialize(struct ata_transport* transport);
