#include "identify.h"

#include <protocol/ahci.h>
#include <stdint.h>
#include <stdio.h>

#include "transport.h"

#define ATA_COMMAND_IDENTIFY_DEVICE 0xecu
#define ATA_IDENTIFY_SIZE 512u

static uint16_t identify_word(const uint8_t* identify, size_t word) {
	return (uint16_t)identify[word * 2u] | (uint16_t)identify[word * 2u + 1u] << 8u;
}

static uint32_t identify_dword(const uint8_t* identify, size_t first_word) {
	return (uint32_t)identify_word(identify, first_word) | (uint32_t)identify_word(identify, first_word + 1u) << 16u;
}

static uint64_t identify_qword(const uint8_t* identify, size_t first_word) {
	return (uint64_t)identify_dword(identify, first_word) | (uint64_t)identify_dword(identify, first_word + 2u) << 32u;
}

static void identify_string(const uint8_t* identify, size_t first_word, size_t word_count, char* output) {
	for (size_t index = 0u; index < word_count; index++) {
		uint8_t first           = identify[(first_word + index) * 2u + 1u];
		uint8_t second          = identify[(first_word + index) * 2u];
		output[index * 2u]      = first >= 0x20u && first <= 0x7eu ? (char)first : '?';
		output[index * 2u + 1u] = second >= 0x20u && second <= 0x7eu ? (char)second : '?';
	}
	size_t size = word_count * 2u;
	while (size != 0u && output[size - 1u] == ' ') size--;
	output[size] = '\0';
}

static uint32_t logical_sector_size(const uint8_t* identify) {
	uint16_t geometry = identify_word(identify, 106u);
	if ((geometry & 0xc000u) == 0x4000u && (geometry & (1u << 12u)) != 0u) {
		uint32_t words = identify_dword(identify, 117u);
		if (words >= 256u && words <= UINT32_MAX / 2u) return words * 2u;
	}
	return 512u;
}

static uint64_t sector_count(const uint8_t* identify, bool* out_lba48) {
	uint16_t commands = identify_word(identify, 83u);
	bool     lba48    = (commands & 0xc000u) == 0x4000u && (commands & (1u << 10u)) != 0u;
	if (out_lba48 != NULL) *out_lba48 = lba48;
	return lba48 ? identify_qword(identify, 100u) : identify_dword(identify, 60u);
}

static const char* sata_speed(const uint8_t* identify) {
	uint16_t capabilities = identify_word(identify, 76u);
	if (capabilities == 0u || capabilities == UINT16_MAX) return "unknown";
	if ((capabilities & (1u << 3u)) != 0u) return "6 Gbit/s";
	if ((capabilities & (1u << 2u)) != 0u) return "3 Gbit/s";
	if ((capabilities & (1u << 1u)) != 0u) return "1.5 Gbit/s";
	return "unknown";
}

static void read_device_info(const uint8_t* identify, struct sata_device_info* info) {
	uint16_t command_sets = identify_word(identify, 83u);
	bool     valid        = (command_sets & 0xc000u) == 0x4000u;

	info->logical_sector_size = logical_sector_size(identify);
	info->sector_count        = sector_count(identify, &info->lba48);
	info->dma                 = (identify_word(identify, 49u) & (1u << 8u)) != 0u;
	info->flush_cache         = valid && (identify_word(identify, 82u) & (1u << 12u)) != 0u;
	info->flush_cache_ext     = valid && (command_sets & (1u << 13u)) != 0u;
}

static void print_identify(uint32_t port, const uint8_t* identify, const struct sata_device_info* info) {
	char     serial[21];
	char     firmware[9];
	char     model[41];
	char     capacity[32];
	uint16_t sata_caps = identify_word(identify, 76u);
	uint16_t rotation  = identify_word(identify, 217u);
	bool     ncq       = sata_caps != 0u && sata_caps != UINT16_MAX && (sata_caps & (1u << 8u)) != 0u;

	identify_string(identify, 10u, 10u, serial);
	identify_string(identify, 23u, 4u, firmware);
	identify_string(identify, 27u, 20u, model);
	if (info->logical_sector_size != 0u && info->sector_count <= UINT64_MAX / info->logical_sector_size)
		(void)snprintf(capacity,
		               sizeof(capacity),
		               "%llu bytes",
		               (unsigned long long)(info->sector_count * info->logical_sector_size));
	else (void)snprintf(capacity, sizeof(capacity), "overflow");
	printf("sata: port %u model=\"%s\" serial=\"%s\" firmware=\"%s\" sectors=%llu sector_size=%u capacity=%s "
	       "lba48=%s ncq=%s queue_depth=%u trim=%s smart=%s speed=%s medium=%s\n",
	       port,
	       model,
	       serial,
	       firmware,
	       (unsigned long long)info->sector_count,
	       info->logical_sector_size,
	       capacity,
	       info->lba48 ? "yes" : "no",
	       ncq ? "yes" : "no",
	       ncq ? (identify_word(identify, 75u) & 0x1fu) + 1u : 1u,
	       (identify_word(identify, 169u) & 1u) != 0u ? "yes" : "no",
	       (identify_word(identify, 82u) & 1u) != 0u ? "yes" : "no",
	       sata_speed(identify),
	       rotation == 1u ? "solid-state" : "rotating/unspecified");
}

enum sata_identify_result sata_identify_device(const struct sata_transport* transport,
                                               struct sata_device_info*     out_info) {
	struct sata_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	struct ahci_transport_execute_response response;
	enum sata_identify_result              result = SATA_IDENTIFY_ERROR;

	if (transport == NULL || out_info == NULL || transport->max_transfer_size < ATA_IDENTIFY_SIZE ||
	    !sata_buffer_allocate(transport, ATA_IDENTIFY_SIZE, &buffer))
		goto done;
	const struct ahci_transport_execute_request request = {
		.header        = {.op = AHCI_TRANSPORT_OP_EXECUTE},
		.buffer_id     = buffer.id,
		.transfer_size = ATA_IDENTIFY_SIZE,
		.direction     = AHCI_DATA_FROM_DEVICE,
		.command       = ATA_COMMAND_IDENTIFY_DEVICE,
	};
	if (!sata_transport_execute(transport, &request, &response) || response.transferred_size != ATA_IDENTIFY_SIZE) {
		fprintf(stderr, "sata: port %u IDENTIFY DEVICE failed\n", transport->port);
		goto done;
	}
	if ((identify_word(buffer.address, 0u) & (1u << 15u)) != 0u) {
		fprintf(stderr, "sata: port %u is not an ATA block device\n", transport->port);
		result = SATA_IDENTIFY_UNSUPPORTED;
		goto done;
	}
	read_device_info(buffer.address, out_info);
	print_identify(transport->port, buffer.address, out_info);
	result = SATA_IDENTIFY_OK;

done:
	sata_buffer_release(transport, &buffer);
	return result;
}
