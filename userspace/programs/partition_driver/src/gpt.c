#include "gpt.h"

#include <runtime/block.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GPT_HEADER_MIN_SIZE 92u
#define GPT_ENTRY_MIN_SIZE 128u
#define GPT_ENTRY_ALIGNMENT 8u
#define GPT_NAME_CODE_UNITS 36u
#define GPT_HEADER_CRC_OFFSET 16u
#define GPT_HEADER_CRC_SIZE 4u

struct gpt_header {
	uint64_t current_lba;
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint64_t entries_lba;
	uint64_t entries_bytes;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t entries_crc;
};

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

static uint64_t read_u64_le(const uint8_t* value) {
	return (uint64_t)read_u32_le(value) | (uint64_t)read_u32_le(value + 4u) << 32u;
}

static bool all_zero(const uint8_t* value, size_t size) {
	for (size_t index = 0u; index < size; index++)
		if (value[index] != 0u) return false;
	return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* bytes, size_t size) {
	for (size_t index = 0u; index < size; index++) {
		crc ^= bytes[index];
		for (size_t bit = 0u; bit < 8u; bit++) crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return crc;
}

static uint32_t header_crc32(const uint8_t* bytes, uint32_t size) {
	static const uint8_t zeros[GPT_HEADER_CRC_SIZE] = {0};
	uint32_t             crc                        = UINT32_MAX;

	crc = crc32_update(crc, bytes, GPT_HEADER_CRC_OFFSET);
	crc = crc32_update(crc, zeros, sizeof(zeros));
	crc = crc32_update(
		crc, bytes + GPT_HEADER_CRC_OFFSET + GPT_HEADER_CRC_SIZE, size - GPT_HEADER_CRC_OFFSET - GPT_HEADER_CRC_SIZE);
	return ~crc;
}

static bool range_contains(uint64_t first, uint64_t count, uint64_t value) {
	return value >= first && value - first < count;
}

static enum gpt_result read_header(const struct block_device* device, uint64_t lba, struct gpt_header* out_header) {
	struct block_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	enum gpt_result result = GPT_RESULT_IO_ERROR;

	if (block_buffer_allocate(device, device->block_size, &buffer) != SYSCALL_STATUS_OK) return result;
	if (block_read(device, &buffer, 0u, lba, 1u) != SYSCALL_STATUS_OK) goto done;
	if (memcmp(buffer.address, "EFI PART", 8u) != 0) {
		result = GPT_RESULT_NOT_PRESENT;
		goto done;
	}
	uint32_t revision     = read_u32_le(buffer.address + 8u);
	uint32_t header_size  = read_u32_le(buffer.address + 12u);
	uint32_t expected_crc = read_u32_le(buffer.address + GPT_HEADER_CRC_OFFSET);
	uint32_t entry_count  = read_u32_le(buffer.address + 80u);
	uint32_t entry_size   = read_u32_le(buffer.address + 84u);
	uint64_t entries_bytes;

	if ((revision >> 16u) != 1u || header_size < GPT_HEADER_MIN_SIZE || header_size > device->block_size ||
	    read_u32_le(buffer.address + 20u) != 0u || header_crc32(buffer.address, header_size) != expected_crc ||
	    all_zero(buffer.address + 56u, 16u) || entry_count == 0u || entry_size < GPT_ENTRY_MIN_SIZE ||
	    entry_size % GPT_ENTRY_ALIGNMENT != 0u || entry_count > UINT64_MAX / entry_size) {
		result = GPT_RESULT_INVALID;
		goto done;
	}
	entries_bytes         = (uint64_t)entry_count * entry_size;
	*out_header           = (struct gpt_header){.current_lba      = read_u64_le(buffer.address + 24u),
	                                            .backup_lba       = read_u64_le(buffer.address + 32u),
	                                            .first_usable_lba = read_u64_le(buffer.address + 40u),
	                                            .last_usable_lba  = read_u64_le(buffer.address + 48u),
	                                            .entries_lba      = read_u64_le(buffer.address + 72u),
	                                            .entries_bytes    = entries_bytes,
	                                            .entry_count      = entry_count,
	                                            .entry_size       = entry_size,
	                                            .entries_crc      = read_u32_le(buffer.address + 88u)};
	uint64_t entry_blocks = entries_bytes / device->block_size + (entries_bytes % device->block_size != 0u);
	if (out_header->current_lba != lba || out_header->backup_lba >= device->block_count ||
	    out_header->backup_lba == out_header->current_lba || out_header->first_usable_lba == 0u ||
	    out_header->first_usable_lba > out_header->last_usable_lba ||
	    out_header->last_usable_lba >= device->block_count || out_header->entries_lba >= device->block_count ||
	    entry_blocks == 0u || entry_blocks > device->block_count - out_header->entries_lba ||
	    range_contains(out_header->entries_lba, entry_blocks, out_header->current_lba) ||
	    !(out_header->entries_lba + entry_blocks <= out_header->first_usable_lba ||
	      out_header->entries_lba > out_header->last_usable_lba) ||
	    range_contains(out_header->first_usable_lba,
	                   out_header->last_usable_lba - out_header->first_usable_lba + 1u,
	                   out_header->current_lba) ||
	    range_contains(out_header->first_usable_lba,
	                   out_header->last_usable_lba - out_header->first_usable_lba + 1u,
	                   out_header->backup_lba)) {
		result = GPT_RESULT_INVALID;
		goto done;
	}
	result = GPT_RESULT_VALID;

done:
	block_buffer_release(device, &buffer);
	return result;
}

static enum gpt_result allocate_entry_buffer(const struct block_device* device, const struct gpt_header* header,
                                             struct block_buffer* out_buffer) {
	uint64_t table_blocks =
		header->entries_bytes / device->block_size + (header->entries_bytes % device->block_size != 0u);
	uint64_t blocks = table_blocks;
	uint64_t size;

	if (blocks > device->max_transfer_blocks) blocks = device->max_transfer_blocks;
	if (blocks > UINT32_MAX / device->block_size) blocks = UINT32_MAX / device->block_size;
	if (blocks == 0u) return GPT_RESULT_INVALID;
	size = blocks * device->block_size;
	if (size < header->entry_size ||
	    (blocks < table_blocks && size < (uint64_t)header->entry_size + device->block_size - 1u))
		return GPT_RESULT_INVALID;
	return block_buffer_allocate(device, (uint32_t)size, out_buffer) == SYSCALL_STATUS_OK ? GPT_RESULT_VALID
	                                                                                      : GPT_RESULT_IO_ERROR;
}

static bool read_entry_batch(const struct block_device* device, struct block_buffer* buffer,
                             const struct gpt_header* header, uint32_t first_index, const uint8_t** out_entries,
                             uint32_t* out_count) {
	uint64_t byte_offset = (uint64_t)first_index * header->entry_size;
	uint64_t block_delta = byte_offset / device->block_size;
	uint32_t offset      = (uint32_t)(byte_offset % device->block_size);
	uint32_t count       = (buffer->size - offset) / header->entry_size;
	uint32_t remaining   = header->entry_count - first_index;

	if (count > remaining) count = remaining;
	if (count == 0u) return false;
	uint64_t span   = offset + (uint64_t)count * header->entry_size;
	uint64_t blocks = span / device->block_size + (span % device->block_size != 0u);
	uint64_t lba;

	if (blocks == 0u || blocks > device->max_transfer_blocks || blocks > UINT32_MAX ||
	    block_delta > UINT64_MAX - header->entries_lba)
		return false;
	lba = header->entries_lba + block_delta;
	if (lba >= device->block_count || blocks > device->block_count - lba ||
	    block_read(device, buffer, 0u, lba, (uint32_t)blocks) != SYSCALL_STATUS_OK)
		return false;
	*out_entries = buffer->address + offset;
	*out_count   = count;
	return true;
}

static bool valid_entry(const uint8_t* entry, const struct gpt_header* header) {
	if (all_zero(entry, 16u)) return true;
	uint64_t first_lba = read_u64_le(entry + 32u);
	uint64_t last_lba  = read_u64_le(entry + 40u);
	return !all_zero(entry + 16u, 16u) && first_lba >= header->first_usable_lba &&
	       last_lba <= header->last_usable_lba && first_lba <= last_lba;
}

static enum gpt_result validate_entries(const struct block_device* device, struct block_buffer* buffer,
                                        const struct gpt_header* header, bool* out_fully_loaded) {
	uint32_t crc = UINT32_MAX;

	*out_fully_loaded = false;
	for (uint32_t index = 0u; index < header->entry_count;) {
		const uint8_t* entries;
		uint32_t       count;
		if (!read_entry_batch(device, buffer, header, index, &entries, &count)) return GPT_RESULT_IO_ERROR;
		if (index == 0u && count == header->entry_count) *out_fully_loaded = true;
		crc = crc32_update(crc, entries, (size_t)count * header->entry_size);
		for (uint32_t offset = 0u; offset < count; offset++) {
			const uint8_t* entry = entries + (size_t)offset * header->entry_size;
			if (!valid_entry(entry, header)) return GPT_RESULT_INVALID;
		}
		index += count;
	}
	return ~crc == header->entries_crc ? GPT_RESULT_VALID : GPT_RESULT_INVALID;
}

static void format_guid(const uint8_t* guid, char output[37]) {
	(void)snprintf(output,
	               37u,
	               "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	               read_u32_le(guid),
	               (unsigned)((uint16_t)guid[4] | (uint16_t)guid[5] << 8u),
	               (unsigned)((uint16_t)guid[6] | (uint16_t)guid[7] << 8u),
	               guid[8],
	               guid[9],
	               guid[10],
	               guid[11],
	               guid[12],
	               guid[13],
	               guid[14],
	               guid[15]);
}

static void format_name(const uint8_t* entry, char output[GPT_NAME_CODE_UNITS + 1u]) {
	size_t size = 0u;
	for (; size < GPT_NAME_CODE_UNITS; size++) {
		uint16_t value = (uint16_t)entry[56u + size * 2u] | (uint16_t)entry[57u + size * 2u] << 8u;
		if (value == 0u) break;
		output[size] = value >= 0x20u && value <= 0x7eu ? (char)value : '?';
	}
	output[size] = '\0';
}

static enum gpt_result print_entries(const struct block_device* device, struct block_buffer* buffer,
                                     const struct gpt_header* header, bool backup, bool fully_loaded) {
	uint32_t populated = 0u;

	printf("partition: valid %sGPT partition table\n", backup ? "backup " : "");
	for (uint32_t index = 0u; index < header->entry_count;) {
		const uint8_t* entries;
		uint32_t       count;
		if (fully_loaded) {
			entries = buffer->address;
			count   = header->entry_count;
		}
		else if (!read_entry_batch(device, buffer, header, index, &entries, &count)) {
			return GPT_RESULT_IO_ERROR;
		}
		for (uint32_t offset = 0u; offset < count; offset++) {
			const uint8_t* entry = entries + (size_t)offset * header->entry_size;
			if (all_zero(entry, 16u)) continue;
			if (!valid_entry(entry, header)) return GPT_RESULT_INVALID;
			uint64_t first_lba = read_u64_le(entry + 32u);
			uint64_t last_lba  = read_u64_le(entry + 40u);
			char     type[37];
			char     name[GPT_NAME_CODE_UNITS + 1u];
			format_guid(entry, type);
			format_name(entry, name);
			printf("partition: GPT entry %u type=%s first_lba=%llu last_lba=%llu blocks=%llu name=\"%s\"\n",
			       index + offset + 1u,
			       type,
			       (unsigned long long)first_lba,
			       (unsigned long long)last_lba,
			       (unsigned long long)(last_lba - first_lba + 1u),
			       name);
			populated++;
		}
		index += count;
	}
	printf("partition: GPT contains %u populated partition entr%s\n", populated, populated == 1u ? "y" : "ies");
	return GPT_RESULT_VALID;
}

static enum gpt_result probe_copy(const struct block_device* device, uint64_t lba, bool backup) {
	struct gpt_header   header;
	struct block_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	bool            fully_loaded;
	enum gpt_result result = read_header(device, lba, &header);
	if (result != GPT_RESULT_VALID) return result;
	result = allocate_entry_buffer(device, &header, &buffer);
	if (result != GPT_RESULT_VALID) return result;
	result = validate_entries(device, &buffer, &header, &fully_loaded);
	if (result == GPT_RESULT_VALID) result = print_entries(device, &buffer, &header, backup, fully_loaded);
	block_buffer_release(device, &buffer);
	return result;
}

enum gpt_result gpt_probe(const struct block_device* device) {
	if (device == NULL || device->block_size < GPT_HEADER_MIN_SIZE || device->block_count < 2u)
		return GPT_RESULT_INVALID;
	enum gpt_result primary = probe_copy(device, 1u, false);
	if (primary == GPT_RESULT_VALID || primary == GPT_RESULT_IO_ERROR) return primary;
	enum gpt_result backup = probe_copy(device, device->block_count - 1u, true);
	if (backup == GPT_RESULT_VALID || backup == GPT_RESULT_IO_ERROR) return backup;
	return primary == GPT_RESULT_NOT_PRESENT && backup == GPT_RESULT_NOT_PRESENT ? GPT_RESULT_NOT_PRESENT
	                                                                             : GPT_RESULT_INVALID;
}
