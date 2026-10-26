#include "directory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAT32_DIRECTORY_ENTRY_SIZE 32u
#define FAT32_DIRECTORY_MAX_SLOTS 65536u
#define FAT32_ATTRIBUTE_VOLUME_ID 0x08u
#define FAT32_ATTRIBUTE_DIRECTORY 0x10u
#define FAT32_ATTRIBUTE_LONG_NAME 0x0fu

static uint16_t read_u16_le(const uint8_t* value) {
	return (uint16_t)value[0] | (uint16_t)value[1] << 8u;
}

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

static char name_character(uint8_t value, bool lowercase) {
	if (value < 0x20u || value > 0x7eu || value == '"' || value == '*' || value == '+' || value == ',' ||
	    value == '/' || value == ':' || value == ';' || value == '<' || value == '=' || value == '>' || value == '?' ||
	    value == '[' || value == '\\' || value == ']' || value == '|')
		return '?';
	if (lowercase && value >= 'A' && value <= 'Z') return (char)(value + ('a' - 'A'));
	return (char)value;
}

static void format_short_name(const uint8_t* entry, char output[FAT32_SHORT_NAME_CAPACITY]) {
	size_t base_size      = 8u;
	size_t extension_size = 3u;
	while (base_size != 0u && entry[base_size - 1u] == ' ') base_size--;
	while (extension_size != 0u && entry[8u + extension_size - 1u] == ' ') extension_size--;
	bool   lowercase_base      = (entry[12u] & 0x08u) != 0u;
	bool   lowercase_extension = (entry[12u] & 0x10u) != 0u;
	size_t position            = 0u;
	for (size_t index = 0u; index < base_size; index++) {
		uint8_t value = entry[index];
		if (index == 0u && value == 0x05u) value = 0xe5u;
		output[position++] = name_character(value, lowercase_base);
	}
	if (extension_size != 0u) {
		output[position++] = '.';
		for (size_t index = 0u; index < extension_size; index++)
			output[position++] = name_character(entry[8u + index], lowercase_extension);
	}
	output[position] = '\0';
}

static bool root_parent_entry(const uint8_t* entry, uint8_t attributes, uint32_t cluster, uint32_t size) {
	if (cluster != 0u || size != 0u || (attributes & FAT32_ATTRIBUTE_DIRECTORY) == 0u || entry[0u] != '.' ||
	    entry[1u] != '.')
		return false;
	for (size_t index = 2u; index < 11u; index++)
		if (entry[index] != ' ') return false;
	return true;
}

static enum fat32_result entry_decode(const struct fat32_volume* volume, const uint8_t* raw,
                                      struct fat32_directory_entry* out_entry) {
	uint8_t attributes = raw[11u];
	if (raw[0u] == 0u || raw[0u] == 0xe5u || attributes == FAT32_ATTRIBUTE_LONG_NAME ||
	    (attributes & FAT32_ATTRIBUTE_VOLUME_ID) != 0u)
		return FAT32_RESULT_NOT_FOUND;
	if ((attributes & 0xc0u) != 0u) return FAT32_RESULT_UNSUPPORTED;
	uint32_t cluster   = ((uint32_t)read_u16_le(raw + 20u) << 16u) | read_u16_le(raw + 26u);
	uint32_t size      = read_u32_le(raw + 28u);
	bool     directory = (attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u;
	if ((cluster == 0u && (directory || size != 0u) && !root_parent_entry(raw, attributes, cluster, size)) ||
	    (cluster != 0u && !fat32_volume_cluster_valid(volume, cluster)))
		return FAT32_RESULT_UNSUPPORTED;
	*out_entry = (struct fat32_directory_entry){
		.cluster    = cluster,
		.size       = size,
		.attributes = attributes,
	};
	format_short_name(raw, out_entry->name);
	return out_entry->name[0u] == '\0' || strchr(out_entry->name, '?') != NULL ? FAT32_RESULT_UNSUPPORTED
	                                                                           : FAT32_RESULT_OK;
}

static bool ascii_equal(const char* left, const void* right, size_t right_size) {
	const uint8_t* bytes = right;
	if (strlen(left) != right_size) return false;
	for (size_t index = 0u; index < right_size; index++) {
		uint8_t a = (uint8_t)left[index];
		uint8_t b = bytes[index];
		if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
		if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
		if (a != b) return false;
	}
	return true;
}

static enum fat32_result scan_directory(struct fat32_volume* volume, uint32_t directory_cluster, const void* name,
                                        size_t name_size, uint64_t wanted, struct fat32_directory_entry* out_entry,
                                        uint64_t* out_total) {
	if (volume == NULL || !fat32_volume_cluster_valid(volume, directory_cluster)) return FAT32_RESULT_UNSUPPORTED;
	uint32_t cluster    = directory_cluster;
	uint64_t live_index = 0u;
	size_t   slots_seen = 0u;
	bool     found      = false;

	for (;;) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* raw = volume->buffer.address + offset;
				if (raw[0u] == 0u) goto complete;
				if (++slots_seen > FAT32_DIRECTORY_MAX_SLOTS) return FAT32_RESULT_UNSUPPORTED;
				struct fat32_directory_entry entry;
				enum fat32_result            decode = entry_decode(volume, raw, &entry);
				if (decode == FAT32_RESULT_NOT_FOUND) continue;
				if (decode != FAT32_RESULT_OK) return decode;
				if ((entry.name[0u] == '.' && entry.name[1u] == '\0') ||
				    (entry.name[0u] == '.' && entry.name[1u] == '.' && entry.name[2u] == '\0'))
					continue;
				if ((!found && name != NULL && ascii_equal(entry.name, name, name_size)) ||
				    (!found && name == NULL && live_index == wanted)) {
					if (out_entry != NULL) *out_entry = entry;
					found = true;
					if (name != NULL) return FAT32_RESULT_OK;
				}
				live_index++;
			}
		}
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) return result;
		if (end) break;
		if (slots_seen / (volume->device.block_size / FAT32_DIRECTORY_ENTRY_SIZE) > volume->cluster_count)
			return FAT32_RESULT_UNSUPPORTED;
		cluster = next;
	}

complete:
	if (out_total != NULL) *out_total = live_index;
	return found ? FAT32_RESULT_OK : FAT32_RESULT_NOT_FOUND;
}

enum fat32_result fat32_directory_lookup(struct fat32_volume* volume, uint32_t directory_cluster, const void* name,
                                         size_t name_size, struct fat32_directory_entry* out_entry) {
	if (name == NULL || name_size == 0u || out_entry == NULL) return FAT32_RESULT_UNSUPPORTED;
	return scan_directory(volume, directory_cluster, name, name_size, 0u, out_entry, NULL);
}

enum fat32_result fat32_directory_enumerate(struct fat32_volume* volume, uint32_t directory_cluster, uint64_t index,
                                            struct fat32_directory_entry* out_entry, uint64_t* out_total) {
	if (out_total == NULL) return FAT32_RESULT_UNSUPPORTED;
	return scan_directory(volume, directory_cluster, NULL, 0u, index, out_entry, out_total);
}

enum fat32_result fat32_file_read(struct fat32_volume* volume, const struct fat32_directory_entry* entry,
                                  uint64_t offset, void* buffer, size_t size, size_t* out_read) {
	if (volume == NULL || entry == NULL || (buffer == NULL && size != 0u) || out_read == NULL ||
	    (entry->attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u)
		return FAT32_RESULT_UNSUPPORTED;
	*out_read = 0u;
	if (offset >= entry->size || size == 0u) return FAT32_RESULT_OK;
	uint64_t available = entry->size - offset;
	if (size > available) size = (size_t)available;
	uint64_t cluster_size = (uint64_t)volume->device.block_size * volume->sectors_per_cluster;
	uint64_t skip         = offset / cluster_size;
	uint32_t cluster      = entry->cluster;
	for (uint64_t index = 0u; index < skip; index++) {
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK || end) return FAT32_RESULT_UNSUPPORTED;
		cluster = next;
	}
	size_t   copied         = 0u;
	uint64_t cluster_offset = offset % cluster_size;
	while (copied < size) {
		uint32_t sector_index  = (uint32_t)(cluster_offset / volume->device.block_size);
		uint32_t sector_offset = (uint32_t)(cluster_offset % volume->device.block_size);
		for (; sector_index < volume->sectors_per_cluster && copied < size; sector_index++) {
			enum fat32_result result =
				fat32_volume_read_sector(volume, fat32_volume_cluster_sector(volume, cluster) + sector_index);
			if (result != FAT32_RESULT_OK) return result;
			size_t chunk = volume->device.block_size - sector_offset;
			if (chunk > size - copied) chunk = size - copied;
			memcpy((uint8_t*)buffer + copied, volume->buffer.address + sector_offset, chunk);
			copied += chunk;
			sector_offset = 0u;
		}
		if (copied == size) break;
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK || end) return FAT32_RESULT_UNSUPPORTED;
		cluster        = next;
		cluster_offset = 0u;
	}
	*out_read = copied;
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_list_root(struct fat32_volume* volume) {
	uint64_t index = 0u;
	uint64_t total = 0u;
	for (;;) {
		struct fat32_directory_entry entry;
		enum fat32_result result = fat32_directory_enumerate(volume, volume->root_cluster, index, &entry, &total);
		if (result == FAT32_RESULT_NOT_FOUND) break;
		if (result != FAT32_RESULT_OK) return result;
		if ((entry.attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u)
			printf("fat32: root directory \"%s\" cluster=%u\n", entry.name, entry.cluster);
		else printf("fat32: root file \"%s\" size=%u cluster=%u\n", entry.name, entry.size, entry.cluster);
		index++;
	}
	printf("fat32: root directory contains %llu live entr%s\n", (unsigned long long)total, total == 1u ? "y" : "ies");
	return FAT32_RESULT_OK;
}
