#include "commands.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <protocol/ahci.h>

#include <ata/transport.h>

#define ATA_COMMAND_PACKET 0xa0u
#define ATA_PACKET_FEATURE_DMA 0x01u
#define SCSI_COMMAND_TEST_UNIT_READY 0x00u
#define SCSI_COMMAND_REQUEST_SENSE 0x03u
#define SCSI_COMMAND_READ_CAPACITY_10 0x25u
#define SCSI_COMMAND_READ_12 0xa8u
#define SCSI_SENSE_RESPONSE_CURRENT 0x70u
#define SCSI_SENSE_RESPONSE_DEFERRED 0x71u
#define SCSI_SENSE_KEY_NOT_READY 0x02u
#define SCSI_SENSE_KEY_UNIT_ATTENTION 0x06u
#define SCSI_ASC_MEDIUM_NOT_PRESENT 0x3au
#define SCSI_REQUEST_SENSE_SIZE 18u

static uint32_t read_be32(const uint8_t* value) {
	return (uint32_t)value[0] << 24u | (uint32_t)value[1] << 16u | (uint32_t)value[2] << 8u | value[3];
}

static void write_be32(uint8_t* output, uint32_t value) {
	output[0] = (uint8_t)(value >> 24u);
	output[1] = (uint8_t)(value >> 16u);
	output[2] = (uint8_t)(value >> 8u);
	output[3] = (uint8_t)value;
}

static bool execute_packet(const struct ata_transport* transport, struct ata_buffer* buffer, uint32_t buffer_offset,
		uint32_t transfer_size, const uint8_t packet[12]) {
	if (transport == NULL || transport->kind != AHCI_DEVICE_SATAPI || packet == NULL ||
			(transfer_size != 0u &&
					(buffer == NULL || buffer_offset > buffer->size || transfer_size > buffer->size - buffer_offset)))
		return false;
	struct ahci_transport_execute_request request = {
		.header = {.op = AHCI_TRANSPORT_OP_EXECUTE},
		.buffer_id = transfer_size == 0u ? 0u : buffer->id,
		.buffer_offset = buffer_offset,
		.transfer_size = transfer_size,
		.direction = transfer_size == 0u ? AHCI_DATA_NONE : AHCI_DATA_FROM_DEVICE,
		.command = ATA_COMMAND_PACKET,
		.feature_low = transfer_size == 0u ? 0u : ATA_PACKET_FEATURE_DMA,
		.packet_size = 12u,
	};
	struct ahci_transport_execute_response response;
	uint32_t byte_count = transfer_size > UINT16_MAX ? UINT16_MAX : transfer_size;
	request.lba[1] = (uint8_t)byte_count;
	request.lba[2] = (uint8_t)(byte_count >> 8u);
	memcpy(request.packet, packet, 12u);
	return ata_transport_execute(transport, &request, &response) && response.transferred_size == transfer_size;
}

static bool request_sense(const struct ata_transport* transport, struct ata_buffer* buffer) {
	uint8_t packet[12] = {0};
	packet[0] = SCSI_COMMAND_REQUEST_SENSE;
	packet[4] = SCSI_REQUEST_SENSE_SIZE;
	return execute_packet(transport, buffer, 0u, SCSI_REQUEST_SENSE_SIZE, packet);
}

enum satapi_media_status satapi_test_media(const struct ata_transport* transport) {
	struct ata_buffer sense = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID
	};
	uint8_t packet[12] = {0};
	enum satapi_media_status status = SATAPI_MEDIA_ERROR;
	if (transport == NULL) goto done;
	packet[0] = SCSI_COMMAND_TEST_UNIT_READY;
	for (size_t attempt = 0u; attempt < 3u; attempt++) {
		if (execute_packet(transport, NULL, 0u, 0u, packet)) {
			status = SATAPI_MEDIA_READY;
			break;
		}
		if (sense.memory_cap == CAP_ID_INVALID && !ata_buffer_allocate(transport, SCSI_REQUEST_SENSE_SIZE, &sense))
			break;
		if (!request_sense(transport, &sense)) break;
		uint8_t response = sense.address[0] & 0x7fu;
		if (response != SCSI_SENSE_RESPONSE_CURRENT && response != SCSI_SENSE_RESPONSE_DEFERRED) break;
		uint8_t sense_key = sense.address[2] & 0x0fu;
		uint8_t additional_sense_code = sense.address[12];
		if (sense_key == SCSI_SENSE_KEY_NOT_READY) {
			status = additional_sense_code == SCSI_ASC_MEDIUM_NOT_PRESENT ? SATAPI_MEDIA_NOT_PRESENT
			                                                              : SATAPI_MEDIA_NOT_READY;
			break;
		}
		if (sense_key != SCSI_SENSE_KEY_UNIT_ATTENTION) break;
	}
done:
	ata_buffer_release(transport, &sense);
	return status;
}

bool satapi_read_capacity(const struct ata_transport* transport, uint64_t* out_block_count, uint32_t* out_block_size) {
	struct ata_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID
	};
	uint8_t packet[12] = {0};
	bool success = false;
	if (transport == NULL || out_block_count == NULL || out_block_size == NULL ||
			!ata_buffer_allocate(transport, 2048u, &buffer))
		goto done;
	packet[0] = SCSI_COMMAND_READ_CAPACITY_10;
	for (size_t attempt = 0u; attempt < 3u; attempt++) {
		if (execute_packet(transport, &buffer, 0u, 8u, packet)) {
			uint32_t last_lba = read_be32(buffer.address);
			uint32_t block_size = read_be32(buffer.address + 4u);
			if (last_lba != UINT32_MAX && block_size != 0u) {
				*out_block_count = (uint64_t)last_lba + 1u;
				*out_block_size = block_size;
				success = true;
				break;
			}
		}
		(void)request_sense(transport, &buffer);
	}
done:
	ata_buffer_release(transport, &buffer);
	return success;
}

bool satapi_read_blocks(const struct ata_transport* transport, struct ata_buffer* buffer, uint32_t buffer_offset,
		uint32_t lba, uint32_t block_count, uint32_t block_size) {
	uint8_t packet[12] = {0};
	if (transport == NULL || block_count == 0u || block_size == 0u || block_count > UINT32_MAX / block_size)
		return false;
	uint32_t transfer_size = block_count * block_size;
	packet[0] = SCSI_COMMAND_READ_12;
	write_be32(packet + 2u, lba);
	write_be32(packet + 6u, block_count);
	return transfer_size <= transport->max_transfer_size &&
	       execute_packet(transport, buffer, buffer_offset, transfer_size, packet);
}
