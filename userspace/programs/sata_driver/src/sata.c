#include "sata.h"

#include <stdio.h>

#include "commands.h"
#include "identify.h"
#include "service.h"
#include "transport.h"

static bool probe_dma_commands(const struct sata_transport* transport, const struct sata_device_info* info) {
	struct sata_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	bool success = false;

	if (!info->dma || info->sector_count == 0u || info->logical_sector_size == 0u ||
	    info->logical_sector_size > transport->max_transfer_size ||
	    !sata_buffer_allocate(transport, info->logical_sector_size, &buffer))
		goto done;
	success = sata_read_dma(transport, &buffer, 0u, 0u, 1u, info->logical_sector_size);
	if (success && info->lba48) success = sata_read_dma_ext(transport, &buffer, 0u, 0u, 1u, info->logical_sector_size);
	if (success && info->flush_cache) success = sata_flush_cache(transport);
	if (success && info->lba48 && info->flush_cache_ext) success = sata_flush_cache_ext(transport);
	if (success) printf("sata: port %u DMA command probe completed\n", transport->port);

done:
	if (!success) fprintf(stderr, "sata: port %u DMA command probe failed\n", transport->port);
	sata_buffer_release(transport, &buffer);
	return success;
}

enum device_driver_result sata_driver_run(cap_id_t device_cap) {
	struct sata_transport     transport = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	struct sata_device_info   info;
	enum sata_identify_result identify;

	if (!sata_transport_initialize(device_cap, &transport)) return DEVICE_DRIVER_ERROR;
	identify = sata_identify_device(&transport, &info);
	if (identify == SATA_IDENTIFY_UNSUPPORTED) {
		sata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	if (identify != SATA_IDENTIFY_OK) {
		sata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_ERROR;
	}
	if (!info.dma || info.sector_count == 0u || info.logical_sector_size == 0u ||
	    info.logical_sector_size > transport.max_transfer_size) {
		fprintf(stderr, "sata: port %u has no usable DMA block geometry\n", transport.port);
		sata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	if (!probe_dma_commands(&transport, &info) || !sata_block_service_run(device_cap, &transport, &info)) {
		sata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_ERROR;
	}
	sata_transport_deinitialize(&transport);
	return DEVICE_DRIVER_ERROR;
}
