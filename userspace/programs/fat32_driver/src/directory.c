#include "directory.h"

#include <runtime/filesystem.h>
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
#define FAT32_LONG_NAME_LAST 0x40u
#define FAT32_LONG_NAME_CHARACTERS_PER_SLOT 13u
#define FAT32_LONG_NAME_MAXIMUM_SLOTS 20u

struct long_name_state {
	uint16_t units[FAT32_LONG_NAME_CHARACTERS_PER_SLOT * FAT32_LONG_NAME_MAXIMUM_SLOTS];
	uint8_t  checksum;
	uint8_t  expected_order;
	uint8_t  slot_count;
	bool     active;
};

static uint16_t read_u16_le(const uint8_t* value) {
	return (uint16_t)value[0] | (uint16_t)value[1] << 8u;
}

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

static void long_name_reset(struct long_name_state* state) {
	*state = (struct long_name_state){0};
}

static uint16_t long_name_unit(const uint8_t* entry, size_t index) {
	static const uint8_t offsets[FAT32_LONG_NAME_CHARACTERS_PER_SLOT] = {
		1u,
		3u,
		5u,
		7u,
		9u,
		14u,
		16u,
		18u,
		20u,
		22u,
		24u,
		28u,
		30u,
	};
	return read_u16_le(entry + offsets[index]);
}

static void long_name_consume(struct long_name_state* state, const uint8_t* entry) {
	uint8_t raw_order = entry[0u];
	uint8_t order     = raw_order & 0x1fu;
	bool    last      = (raw_order & FAT32_LONG_NAME_LAST) != 0u;

	if ((raw_order & 0xa0u) != 0u || order == 0u || order > FAT32_LONG_NAME_MAXIMUM_SLOTS || entry[12u] != 0u ||
	    read_u16_le(entry + 26u) != 0u) {
		long_name_reset(state);
		return;
	}
	if (last) {
		long_name_reset(state);
		state->active         = true;
		state->checksum       = entry[13u];
		state->expected_order = order;
		state->slot_count     = order;
	}
	if (!state->active || state->checksum != entry[13u] || state->expected_order != order) {
		long_name_reset(state);
		return;
	}
	size_t base = (size_t)(order - 1u) * FAT32_LONG_NAME_CHARACTERS_PER_SLOT;
	for (size_t index = 0u; index < FAT32_LONG_NAME_CHARACTERS_PER_SLOT; index++)
		state->units[base + index] = long_name_unit(entry, index);
	state->expected_order--;
}

static uint8_t short_name_checksum(const uint8_t* entry) {
	uint8_t checksum = 0u;
	for (size_t index = 0u; index < 11u; index++)
		checksum = (uint8_t)(((checksum & 1u) != 0u ? 0x80u : 0u) + (checksum >> 1u) + entry[index]);
	return checksum;
}

static bool append_utf8(uint32_t codepoint, char output[FILESYSTEM_NAME_MAX + 1u], size_t* size) {
	uint8_t bytes[4];
	size_t  count;
	if (codepoint <= 0x7fu) {
		bytes[0u] = (uint8_t)codepoint;
		count     = 1u;
	}
	else if (codepoint <= 0x7ffu) {
		bytes[0u] = (uint8_t)(0xc0u | codepoint >> 6u);
		bytes[1u] = (uint8_t)(0x80u | (codepoint & 0x3fu));
		count     = 2u;
	}
	else if (codepoint <= 0xffffu) {
		if (codepoint >= 0xd800u && codepoint <= 0xdfffu) return false;
		bytes[0u] = (uint8_t)(0xe0u | codepoint >> 12u);
		bytes[1u] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
		bytes[2u] = (uint8_t)(0x80u | (codepoint & 0x3fu));
		count     = 3u;
	}
	else if (codepoint <= 0x10ffffu) {
		bytes[0u] = (uint8_t)(0xf0u | codepoint >> 18u);
		bytes[1u] = (uint8_t)(0x80u | ((codepoint >> 12u) & 0x3fu));
		bytes[2u] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
		bytes[3u] = (uint8_t)(0x80u | (codepoint & 0x3fu));
		count     = 4u;
	}
	else return false;
	if (*size > FILESYSTEM_NAME_MAX - count) return false;
	for (size_t index = 0u; index < count; index++) output[(*size)++] = (char)bytes[index];
	return true;
}

static bool long_name_decode(const struct long_name_state* state, const uint8_t* short_entry,
                             char output[FILESYSTEM_NAME_MAX + 1u]) {
	if (!state->active || state->expected_order != 0u || state->slot_count == 0u ||
	    state->checksum != short_name_checksum(short_entry))
		return false;
	size_t size       = 0u;
	size_t unit_count = (size_t)state->slot_count * FAT32_LONG_NAME_CHARACTERS_PER_SLOT;
	bool   terminated = false;
	for (size_t index = 0u; index < unit_count; index++) {
		uint16_t unit = state->units[index];
		if (terminated) {
			if (unit != 0xffffu) return false;
			continue;
		}
		if (unit == 0u) {
			terminated = true;
			continue;
		}
		if (unit == 0xffffu || index >= FILESYSTEM_NAME_MAX) return false;
		uint32_t codepoint = unit;
		if (unit >= 0xd800u && unit <= 0xdbffu) {
			if (++index >= unit_count) return false;
			uint16_t low = state->units[index];
			if (low < 0xdc00u || low > 0xdfffu) return false;
			codepoint = 0x10000u + ((uint32_t)(unit - 0xd800u) << 10u) + (low - 0xdc00u);
		}
		else if (unit >= 0xdc00u && unit <= 0xdfffu) return false;
		if (!append_utf8(codepoint, output, &size)) return false;
	}
	if (size == 0u || !filesystem_relative_path_valid(output, size)) return false;
	output[size] = '\0';
	return true;
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
	uint32_t               cluster    = directory_cluster;
	uint64_t               live_index = 0u;
	size_t                 slots_seen = 0u;
	bool                   found      = false;
	struct long_name_state long_name  = {0};

	for (;;) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* raw = volume->buffer.address + offset;
				if (raw[0u] == 0u) goto complete;
				if (++slots_seen > FAT32_DIRECTORY_MAX_SLOTS) return FAT32_RESULT_UNSUPPORTED;
				if (raw[11u] == FAT32_ATTRIBUTE_LONG_NAME) {
					if (raw[0u] == 0xe5u) long_name_reset(&long_name);
					else long_name_consume(&long_name, raw);
					continue;
				}
				struct fat32_directory_entry entry;
				enum fat32_result            decode = entry_decode(volume, raw, &entry);
				if (decode == FAT32_RESULT_NOT_FOUND) {
					long_name_reset(&long_name);
					continue;
				}
				if (decode != FAT32_RESULT_OK) return decode;
				char short_name[FAT32_SHORT_NAME_CAPACITY];
				memcpy(short_name, entry.name, strlen(entry.name) + 1u);
				char decoded_name[FILESYSTEM_NAME_MAX + 1u];
				bool have_long_name = long_name_decode(&long_name, raw, decoded_name);
				if (have_long_name) memcpy(entry.name, decoded_name, strlen(decoded_name) + 1u);
				long_name_reset(&long_name);
				if ((entry.name[0u] == '.' && entry.name[1u] == '\0') ||
				    (entry.name[0u] == '.' && entry.name[1u] == '.' && entry.name[2u] == '\0'))
					continue;
				if ((!found && name != NULL &&
				     (ascii_equal(entry.name, name, name_size) ||
				      (have_long_name && ascii_equal(short_name, name, name_size)))) ||
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
