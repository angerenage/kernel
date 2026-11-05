#include "gpt.h"

#include <runtime/block.h>
#include <runtime/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "table.h"

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

struct gpt_buffers {
	struct block_buffer data;
	struct block_buffer scratch;
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

static void buffers_release(const struct block_device* device, struct gpt_buffers* buffers) {
	block_buffer_release(device, &buffers->scratch);
	block_buffer_release(device, &buffers->data);
}

static enum gpt_result buffers_allocate(const struct block_device* device, uint32_t data_size,
                                        struct gpt_buffers* buffers) {
	*buffers = (struct gpt_buffers){
		.data    = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID},
		.scratch = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID},
	};
	if (block_buffer_allocate(device, data_size, &buffers->data) != SYSCALL_STATUS_OK ||
	    block_buffer_allocate(device, device->block_size, &buffers->scratch) != SYSCALL_STATUS_OK) {
		buffers_release(device, buffers);
		return GPT_RESULT_IO_ERROR;
	}
	return GPT_RESULT_VALID;
}

static enum gpt_result read_header(const struct block_device* device, uint32_t sector_size, uint64_t disk_sectors,
                                   uint64_t lba, struct gpt_header* out_header) {
	struct gpt_buffers buffers;
	enum gpt_result    result = buffers_allocate(device, sector_size, &buffers);
	if (result != GPT_RESULT_VALID) return result;
	if (block_read(device, &buffers.data, 0u, &buffers.scratch, lba * sector_size, sector_size) != SYSCALL_STATUS_OK)
		goto done;
	if (memcmp(buffers.data.address, "EFI PART", 8u) != 0) {
		result = GPT_RESULT_NOT_PRESENT;
		goto done;
	}
	uint32_t revision     = read_u32_le(buffers.data.address + 8u);
	uint32_t header_size  = read_u32_le(buffers.data.address + 12u);
	uint32_t expected_crc = read_u32_le(buffers.data.address + GPT_HEADER_CRC_OFFSET);
	uint32_t entry_count  = read_u32_le(buffers.data.address + 80u);
	uint32_t entry_size   = read_u32_le(buffers.data.address + 84u);
	if ((revision >> 16u) != 1u || header_size < GPT_HEADER_MIN_SIZE || header_size > sector_size ||
	    read_u32_le(buffers.data.address + 20u) != 0u ||
	    header_crc32(buffers.data.address, header_size) != expected_crc || all_zero(buffers.data.address + 56u, 16u) ||
	    entry_count == 0u || entry_size < GPT_ENTRY_MIN_SIZE || entry_size % GPT_ENTRY_ALIGNMENT != 0u ||
	    entry_count > UINT64_MAX / entry_size) {
		result = GPT_RESULT_INVALID;
		goto done;
	}
	*out_header = (struct gpt_header){
		.current_lba      = read_u64_le(buffers.data.address + 24u),
		.backup_lba       = read_u64_le(buffers.data.address + 32u),
		.first_usable_lba = read_u64_le(buffers.data.address + 40u),
		.last_usable_lba  = read_u64_le(buffers.data.address + 48u),
		.entries_lba      = read_u64_le(buffers.data.address + 72u),
		.entries_bytes    = (uint64_t)entry_count * entry_size,
		.entry_count      = entry_count,
		.entry_size       = entry_size,
		.entries_crc      = read_u32_le(buffers.data.address + 88u),
	};
	uint64_t entry_sectors = out_header->entries_bytes / sector_size + (out_header->entries_bytes % sector_size != 0u);
	if (out_header->current_lba != lba || out_header->backup_lba >= disk_sectors ||
	    out_header->backup_lba == out_header->current_lba || out_header->first_usable_lba == 0u ||
	    out_header->first_usable_lba > out_header->last_usable_lba || out_header->last_usable_lba >= disk_sectors ||
	    out_header->entries_lba >= disk_sectors || entry_sectors == 0u ||
	    entry_sectors > disk_sectors - out_header->entries_lba ||
	    range_contains(out_header->entries_lba, entry_sectors, out_header->current_lba) ||
	    !(out_header->entries_lba + entry_sectors <= out_header->first_usable_lba ||
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
	buffers_release(device, &buffers);
	return result;
}

static enum gpt_result allocate_entry_buffers(const struct block_device* device, const struct gpt_header* header,
                                              struct gpt_buffers* buffers) {
	uint64_t transfer_bytes = (uint64_t)device->max_transfer_blocks * device->block_size;
	uint64_t size           = header->entries_bytes < transfer_bytes ? header->entries_bytes : transfer_bytes;
	if (size > UINT32_MAX) size = UINT32_MAX;
	uint32_t count = (uint32_t)(size / header->entry_size);
	if (count == 0u) return GPT_RESULT_INVALID;
	return buffers_allocate(device, count * header->entry_size, buffers);
}

static bool read_entry_batch(const struct block_device* device, struct gpt_buffers* buffers,
                             const struct gpt_header* header, uint32_t sector_size, uint32_t first_index,
                             uint32_t* out_count) {
	uint32_t count     = buffers->data.size / header->entry_size;
	uint32_t remaining = header->entry_count - first_index;
	if (count > remaining) count = remaining;
	uint64_t table_offset = header->entries_lba * sector_size;
	uint64_t entry_offset = (uint64_t)first_index * header->entry_size;
	if (count == 0u || table_offset > UINT64_MAX - entry_offset) return false;
	uint32_t size = count * header->entry_size;
	if (block_read(device, &buffers->data, 0u, &buffers->scratch, table_offset + entry_offset, size) !=
	    SYSCALL_STATUS_OK)
		return false;
	*out_count = count;
	return true;
}

static bool valid_entry(const uint8_t* entry, const struct gpt_header* header) {
	if (all_zero(entry, 16u)) return true;
	uint64_t first_lba = read_u64_le(entry + 32u);
	uint64_t last_lba  = read_u64_le(entry + 40u);
	return !all_zero(entry + 16u, 16u) && first_lba >= header->first_usable_lba &&
	       last_lba <= header->last_usable_lba && first_lba <= last_lba;
}

static enum gpt_result validate_entries(const struct block_device* device, struct gpt_buffers* buffers,
                                        const struct gpt_header* header, uint32_t sector_size) {
	uint32_t crc = UINT32_MAX;
	for (uint32_t index = 0u; index < header->entry_count;) {
		uint32_t count;
		if (!read_entry_batch(device, buffers, header, sector_size, index, &count)) return GPT_RESULT_IO_ERROR;
		crc = crc32_update(crc, buffers->data.address, (size_t)count * header->entry_size);
		for (uint32_t offset = 0u; offset < count; offset++)
			if (!valid_entry(buffers->data.address + (size_t)offset * header->entry_size, header))
				return GPT_RESULT_INVALID;
		index += count;
	}
	return ~crc == header->entries_crc ? GPT_RESULT_VALID : GPT_RESULT_INVALID;
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

static enum gpt_result collect_entries(const struct block_device* device, struct gpt_buffers* buffers,
                                       const struct gpt_header* header, uint32_t sector_size, bool backup,
                                       struct partition_table* table) {
	uint32_t populated = 0u;
	for (uint32_t index = 0u; index < header->entry_count;) {
		uint32_t count;
		if (!read_entry_batch(device, buffers, header, sector_size, index, &count)) return GPT_RESULT_IO_ERROR;
		for (uint32_t offset = 0u; offset < count; offset++) {
			const uint8_t* entry = buffers->data.address + (size_t)offset * header->entry_size;
			if (all_zero(entry, 16u)) continue;
			uint64_t               first_lba = read_u64_le(entry + 32u);
			uint64_t               last_lba  = read_u64_le(entry + 40u);
			struct partition_entry partition = {
				.scheme      = PARTITION_SCHEME_GPT,
				.table_index = index + offset + 1u,
				.first_lba   = first_lba,
				.block_count = last_lba - first_lba + 1u,
			};
			char type[PARTITION_GUID_STRING_SIZE];
			memcpy(partition.gpt.type_guid, entry, sizeof(partition.gpt.type_guid));
			memcpy(partition.gpt.unique_guid, entry + 16u, sizeof(partition.gpt.unique_guid));
			partition.gpt.attributes = read_u64_le(entry + 48u);
			format_name(entry, partition.gpt.name);
			if (!partition_table_append(table, &partition)) return GPT_RESULT_NO_MEMORY;
			partition_format_guid(partition.gpt.type_guid, type);
			RUNTIME_DEBUG_LOG("partition: GPT entry %u type=%s first_lba=%llu last_lba=%llu blocks=%llu name=\"%s\"\n",
			                  partition.table_index,
			                  type,
			                  (unsigned long long)first_lba,
			                  (unsigned long long)last_lba,
			                  (unsigned long long)partition.block_count,
			                  partition.gpt.name);
			populated++;
		}
		index += count;
	}
	printf("partition: %sGPT table contains %u partition%s (sector size %u)\n",
	       backup ? "backup " : "",
	       populated,
	       populated == 1u ? "" : "s",
	       sector_size);
	return GPT_RESULT_VALID;
}

static enum gpt_result probe_copy(const struct block_device* device, uint32_t sector_size, uint64_t disk_sectors,
                                  uint64_t lba, bool backup, struct partition_table* table) {
	struct gpt_header header;
	enum gpt_result   result = read_header(device, sector_size, disk_sectors, lba, &header);
	if (result != GPT_RESULT_VALID) return result;
	struct gpt_buffers buffers;
	result = allocate_entry_buffers(device, &header, &buffers);
	if (result != GPT_RESULT_VALID) return result;
	result = validate_entries(device, &buffers, &header, sector_size);
	if (result == GPT_RESULT_VALID) result = collect_entries(device, &buffers, &header, sector_size, backup, table);
	buffers_release(device, &buffers);
	return result;
}

static enum gpt_result probe_geometry(const struct block_device* device, uint32_t sector_size,
                                      struct partition_table* table) {
	uint64_t disk_bytes = device->block_count * device->block_size;
	if (sector_size < GPT_HEADER_MIN_SIZE || disk_bytes % sector_size != 0u) return GPT_RESULT_NOT_PRESENT;
	uint64_t disk_sectors = disk_bytes / sector_size;
	if (disk_sectors < 2u) return GPT_RESULT_NOT_PRESENT;
	enum gpt_result primary = probe_copy(device, sector_size, disk_sectors, 1u, false, table);
	if (primary == GPT_RESULT_VALID || primary == GPT_RESULT_IO_ERROR || primary == GPT_RESULT_NO_MEMORY)
		return primary;
	table->count           = 0u;
	enum gpt_result backup = probe_copy(device, sector_size, disk_sectors, disk_sectors - 1u, true, table);
	if (backup == GPT_RESULT_VALID || backup == GPT_RESULT_IO_ERROR || backup == GPT_RESULT_NO_MEMORY) return backup;
	table->count = 0u;
	return primary == GPT_RESULT_NOT_PRESENT && backup == GPT_RESULT_NOT_PRESENT ? GPT_RESULT_NOT_PRESENT
	                                                                             : GPT_RESULT_INVALID;
}

enum gpt_result gpt_probe(const struct block_device* device, struct partition_table* table) {
	if (device == NULL || table == NULL || table->count != 0u || device->block_size == 0u ||
	    device->block_count > UINT64_MAX / device->block_size)
		return GPT_RESULT_INVALID;
	uint32_t        sector_sizes[2] = {device->block_size, 512u};
	enum gpt_result result          = GPT_RESULT_NOT_PRESENT;
	bool            invalid         = false;
	for (size_t index = 0u; index < 2u; index++) {
		if (index != 0u && sector_sizes[index] == sector_sizes[0]) continue;
		result = probe_geometry(device, sector_sizes[index], table);
		if (result == GPT_RESULT_VALID) {
			table->block_size = sector_sizes[index];
			return result;
		}
		if (result == GPT_RESULT_IO_ERROR || result == GPT_RESULT_NO_MEMORY) return result;
		if (result == GPT_RESULT_INVALID) invalid = true;
	}
	return invalid ? GPT_RESULT_INVALID : result;
}
