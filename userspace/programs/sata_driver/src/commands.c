#include "commands.h"

#include <limits.h>
#include <protocol/ahci.h>
#include <stddef.h>

#include "transport.h"

#define ATA_COMMAND_READ_DMA_EXT 0x25u
#define ATA_COMMAND_WRITE_DMA_EXT 0x35u
#define ATA_COMMAND_FLUSH_CACHE_EXT 0xeau
#define ATA_COMMAND_READ_DMA 0xc8u
#define ATA_COMMAND_WRITE_DMA 0xcau
#define ATA_COMMAND_FLUSH_CACHE 0xe7u
#define ATA_DEVICE_LBA (1u << 6u)
#define ATA_LBA28_LIMIT ((uint64_t)1u << 28u)
#define ATA_LBA48_LIMIT ((uint64_t)1u << 48u)

static bool execute_dma(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                        uint64_t lba, uint16_t sector_count, uint32_t sector_size, bool extended, bool write) {
	struct ahci_transport_execute_request request = {
		.header        = {.op = AHCI_TRANSPORT_OP_EXECUTE},
		.buffer_id     = buffer == NULL ? 0u : buffer->id,
		.buffer_offset = buffer_offset,
		.direction     = write ? AHCI_DATA_TO_DEVICE : AHCI_DATA_FROM_DEVICE,
		.command       = extended ? (write ? ATA_COMMAND_WRITE_DMA_EXT : ATA_COMMAND_READ_DMA_EXT)
		                          : (write ? ATA_COMMAND_WRITE_DMA : ATA_COMMAND_READ_DMA),
		.device        = ATA_DEVICE_LBA,
	};
	struct ahci_transport_execute_response response;
	uint32_t                               transfer_size;
	uint64_t                               limit = extended ? ATA_LBA48_LIMIT : ATA_LBA28_LIMIT;

	if (transport == NULL || buffer == NULL || sector_size == 0u || sector_count == 0u ||
	    (!extended && sector_count > 256u) || lba >= limit || sector_count > limit - lba ||
	    sector_count > UINT32_MAX / sector_size)
		return false;
	transfer_size = (uint32_t)sector_count * sector_size;
	if (transfer_size > transport->max_transfer_size || buffer_offset > buffer->size ||
	    transfer_size > buffer->size - buffer_offset)
		return false;
	request.transfer_size = transfer_size;
	for (size_t index = 0u; index < (extended ? 6u : 3u); index++) request.lba[index] = (uint8_t)(lba >> (index * 8u));
	if (extended) request.count = sector_count;
	else {
		request.device |= (uint8_t)(lba >> 24u) & 0x0fu;
		request.count = sector_count == 256u ? 0u : (uint8_t)sector_count;
	}
	return sata_transport_execute(transport, &request, &response) && response.transferred_size == transfer_size;
}

static bool execute_flush(const struct sata_transport* transport, uint8_t command) {
	const struct ahci_transport_execute_request request = {
		.header = {.op = AHCI_TRANSPORT_OP_EXECUTE}, .direction = AHCI_DATA_NONE, .command = command};
	struct ahci_transport_execute_response response;

	return transport != NULL && sata_transport_execute(transport, &request, &response) &&
	       response.transferred_size == 0u;
}

bool sata_read_dma_ext(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                       uint64_t lba, uint16_t sector_count, uint32_t sector_size) {
	return execute_dma(transport, buffer, buffer_offset, lba, sector_count, sector_size, true, false);
}

bool sata_write_dma_ext(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                        uint64_t lba, uint16_t sector_count, uint32_t sector_size) {
	return execute_dma(transport, buffer, buffer_offset, lba, sector_count, sector_size, true, true);
}

bool sata_flush_cache_ext(const struct sata_transport* transport) {
	return execute_flush(transport, ATA_COMMAND_FLUSH_CACHE_EXT);
}

bool sata_read_dma(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                   uint32_t lba, uint16_t sector_count, uint32_t sector_size) {
	return execute_dma(transport, buffer, buffer_offset, lba, sector_count, sector_size, false, false);
}

bool sata_write_dma(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                    uint32_t lba, uint16_t sector_count, uint32_t sector_size) {
	return execute_dma(transport, buffer, buffer_offset, lba, sector_count, sector_size, false, true);
}

bool sata_flush_cache(const struct sata_transport* transport) {
	return execute_flush(transport, ATA_COMMAND_FLUSH_CACHE);
}
