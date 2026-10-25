#pragma once

#include <base/cap.h>
#include <protocol/ahci.h>
#include <stdbool.h>
#include <stdint.h>

/* One client mapping of storage allocated and DMA-mapped by AHCI. */
struct sata_buffer {
	uint64_t id;
	cap_id_t memory_cap;
	cap_id_t address_space_cap;
	cap_id_t mapping_cap;
	uint8_t* address;
	uint32_t size;
};

/* One exclusive session with an AHCI SATA port. */
struct sata_transport {
	cap_id_t root_cap;
	cap_id_t session_cap;
	uint32_t port;
	uint32_t max_transfer_size;
};

/* Acquire the child resource and open a SATA transport session. */
bool sata_transport_initialize(cap_id_t device_cap, struct sata_transport* out_transport);

/* Allocate and map a buffer suitable for commands on this transport. */
bool sata_buffer_allocate(const struct sata_transport* transport, uint32_t size, struct sata_buffer* out_buffer);

/* Execute one synchronous ATA command through AHCI. */
bool sata_transport_execute(const struct sata_transport*                 transport,
                            const struct ahci_transport_execute_request* request,
                            struct ahci_transport_execute_response*      out_response);

/* Unmap and release an AHCI-owned DMA buffer. */
void sata_buffer_release(const struct sata_transport* transport, struct sata_buffer* buffer);

/* Close a transport session after all buffers have been released. */
void sata_transport_deinitialize(struct sata_transport* transport);
