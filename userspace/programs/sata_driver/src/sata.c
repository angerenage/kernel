#include "sata.h"

#include <stdio.h>

#include <ata/block_service.h>
#include <ata/transport.h>

#include "commands.h"
#include "identify.h"

struct sata_backend {
	const struct ata_transport* transport;
	const struct sata_device_info* info;
};

static uint32_t transfer_block_limit(const struct ata_transport* transport, const struct sata_device_info* info) {
	uint32_t count = transport->max_transfer_size / info->logical_sector_size;
	uint32_t limit = info->lba48 ? UINT16_MAX : 256u;
	return count < limit ? count : limit;
}

static bool transfer(void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index,
		uint32_t block_count, bool write) {
	const struct sata_backend* backend = context;
	if (backend->info->lba48)
		return write ? sata_write_dma_ext(backend->transport,
							   buffer,
							   buffer_offset,
							   block_index,
							   (uint16_t)block_count,
							   backend->info->logical_sector_size)
		             : sata_read_dma_ext(backend->transport,
							   buffer,
							   buffer_offset,
							   block_index,
							   (uint16_t)block_count,
							   backend->info->logical_sector_size);
	return write ? sata_write_dma(backend->transport,
						   buffer,
						   buffer_offset,
						   (uint32_t)block_index,
						   (uint16_t)block_count,
						   backend->info->logical_sector_size)
	             : sata_read_dma(backend->transport,
						   buffer,
						   buffer_offset,
						   (uint32_t)block_index,
						   (uint16_t)block_count,
						   backend->info->logical_sector_size);
}

static bool read_blocks(
		void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index, uint32_t block_count) {
	return transfer(context, buffer, buffer_offset, block_index, block_count, false);
}

static bool write_blocks(
		void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index, uint32_t block_count) {
	return transfer(context, buffer, buffer_offset, block_index, block_count, true);
}

static bool flush(void* context) {
	const struct sata_backend* backend = context;
	if (backend->info->lba48 && backend->info->flush_cache_ext) return sata_flush_cache_ext(backend->transport);
	return sata_flush_cache(backend->transport);
}

static bool probe_dma_commands(const struct ata_transport* transport, const struct sata_device_info* info) {
	struct ata_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID
	};
	bool success = false;

	if (!info->dma || info->sector_count == 0u || info->logical_sector_size == 0u ||
			info->logical_sector_size > transport->max_transfer_size ||
			!ata_buffer_allocate(transport, info->logical_sector_size, &buffer))
		goto done;
	success = sata_read_dma(transport, &buffer, 0u, 0u, 1u, info->logical_sector_size);
	if (success && info->lba48) success = sata_read_dma_ext(transport, &buffer, 0u, 0u, 1u, info->logical_sector_size);
	if (success && info->flush_cache) success = sata_flush_cache(transport);
	if (success && info->lba48 && info->flush_cache_ext) success = sata_flush_cache_ext(transport);

done:
	if (!success) fprintf(stderr, "sata: port %u DMA command probe failed\n", transport->port);
	ata_buffer_release(transport, &buffer);
	return success;
}

enum device_driver_result sata_driver_run(cap_id_t device_cap) {
	struct ata_transport transport = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	struct sata_device_info info;
	enum sata_identify_result identify;

	if (!ata_transport_initialize(device_cap, AHCI_DEVICE_SATA, &transport)) return DEVICE_DRIVER_ERROR;
	identify = sata_identify_device(&transport, &info);
	if (identify == SATA_IDENTIFY_UNSUPPORTED) {
		ata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	if (identify != SATA_IDENTIFY_OK) {
		ata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_ERROR;
	}
	if (!info.dma || info.sector_count == 0u || info.logical_sector_size == 0u ||
			info.logical_sector_size > transport.max_transfer_size) {
		fprintf(stderr, "sata: port %u has no usable DMA block geometry\n", transport.port);
		ata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	struct sata_backend backend = {.transport = &transport, .info = &info};
	struct ata_block_info block_info = {
		.name = "sata",
		.compatible = NULL,
		.block_count = info.sector_count,
		.block_size = info.logical_sector_size,
		.max_transfer_blocks = transfer_block_limit(&transport, &info),
		.writable = true,
		.removable = false,
		.flush_supported = info.flush_cache || info.flush_cache_ext
	};
	struct ata_block_backend block_backend = {
		.context = &backend, .read = read_blocks, .write = write_blocks, .flush = flush
	};
	if (!probe_dma_commands(&transport, &info) ||
			!ata_block_service_run(device_cap, &transport, &block_info, &block_backend)) {
		ata_transport_deinitialize(&transport);
		return DEVICE_DRIVER_ERROR;
	}
	ata_transport_deinitialize(&transport);
	return DEVICE_DRIVER_ERROR;
}
