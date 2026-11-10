#include "satapi.h"

#include <ata/block_service.h>
#include <ata/transport.h>
#include <protocol/ahci.h>
#include <stdint.h>
#include <stdio.h>

#include "commands.h"

struct satapi_backend {
	const struct ata_transport* transport;
	uint32_t                    block_size;
};

static bool read_blocks(void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index,
                        uint32_t block_count) {
	struct satapi_backend* backend = context;
	return block_index <= UINT32_MAX &&
	       satapi_read_blocks(
			   backend->transport, buffer, buffer_offset, (uint32_t)block_index, block_count, backend->block_size);
}

enum device_driver_result satapi_driver_run(cap_id_t device_cap) {
	struct ata_transport  transport = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	struct satapi_backend backend   = {.transport = &transport};
	struct ata_buffer     probe     = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	uint64_t physical_block_count;
	uint32_t physical_block_size;

	if (!ata_transport_initialize(device_cap, AHCI_DEVICE_SATAPI, &transport)) return DEVICE_DRIVER_ERROR;
	if (!satapi_read_capacity(&transport, &physical_block_count, &physical_block_size)) {
		fprintf(stderr, "satapi: port %u READ CAPACITY failed\n", transport.port);
		goto error;
	}
	if (physical_block_count > UINT32_MAX || physical_block_size > transport.max_transfer_size) {
		fprintf(stderr, "satapi: port %u has unsupported media geometry\n", transport.port);
		ata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	backend.block_size = physical_block_size;
	if (!ata_buffer_allocate(&transport, physical_block_size, &probe) ||
	    !satapi_read_blocks(&transport, &probe, 0u, 0u, 1u, physical_block_size)) {
		fprintf(stderr, "satapi: port %u media read probe failed\n", transport.port);
		goto error;
	}
	struct ata_block_info block_info = {
		.name                = "cdrom",
		.compatible          = NULL,
		.block_count         = physical_block_count,
		.block_size          = physical_block_size,
		.max_transfer_blocks = transport.max_transfer_size / physical_block_size,
		.writable            = false,
		.removable           = true,
		.flush_supported     = false,
	};
	struct ata_block_backend block_backend = {.context = &backend, .read = read_blocks};
	printf("satapi: port %u media has %llu native blocks of %u bytes\n",
	       transport.port,
	       (unsigned long long)physical_block_count,
	       physical_block_size);
	if (!ata_block_service_run(device_cap, &transport, &block_info, &block_backend)) goto error;
	ata_buffer_release(&transport, &probe);
	ata_transport_deinitialize(&transport);
	return DEVICE_DRIVER_ERROR;

error:
	ata_buffer_release(&transport, &probe);
	ata_transport_deinitialize(&transport);
	return DEVICE_DRIVER_ERROR;
}
