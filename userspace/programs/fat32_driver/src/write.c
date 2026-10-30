#include "write.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "directory.h"

#define FAT32_DIRECTORY_ENTRY_SIZE 32u
#define FAT32_DIRECTORY_MAX_SLOTS 65536u
#define FAT32_ATTRIBUTE_READ_ONLY 0x01u
#define FAT32_ATTRIBUTE_VOLUME_ID 0x08u
#define FAT32_ATTRIBUTE_DIRECTORY 0x10u
#define FAT32_ATTRIBUTE_LONG_NAME 0x0fu
#define FAT32_ATTRIBUTE_ARCHIVE 0x20u
#define FAT32_END_OF_CHAIN 0x0fffffffu
#define FAT32_LONG_NAME_LAST 0x40u
#define FAT32_LONG_NAME_CHARACTERS_PER_SLOT 13u
#define FAT32_LONG_NAME_MAXIMUM_SLOTS 20u
#define FAT32_WRITE_MAX_FILE_SIZE (16u * 1024u * 1024u)

struct directory_slot {
	uint64_t sector;
	uint32_t offset;
};

struct root_entry {
	uint64_t sector;
	uint32_t offset;
	uint32_t cluster;
	uint32_t size;
	uint32_t tail_cluster;
	uint8_t  attributes;
	bool     found;
	bool     slot_available;
};

static int compare_clusters(const void* left, const void* right) {
	uint32_t a = *(const uint32_t*)left;
	uint32_t b = *(const uint32_t*)right;
	return a < b ? -1 : a != b;
}

static uint16_t read_u16_le(const uint8_t* value) {
	return (uint16_t)value[0] | (uint16_t)value[1] << 8u;
}

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

static void write_u16_le(uint8_t* output, uint16_t value) {
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t* output, uint32_t value) {
	for (size_t index = 0u; index < sizeof(value); index++) output[index] = (uint8_t)(value >> (index * 8u));
}

static uint8_t short_name_checksum(const uint8_t name[11]) {
	uint8_t checksum = 0u;
	for (size_t index = 0u; index < 11u; index++)
		checksum = (uint8_t)(((checksum & 1u) != 0u ? 0x80u : 0u) + (checksum >> 1u) + name[index]);
	return checksum;
}

static bool fat_name_codepoint_valid(uint32_t codepoint) {
	if (codepoint < 0x20u || codepoint == 0x7fu) return false;
	if (codepoint > 0x7fu) return true;
	return strchr("\"*/:<>?\\|", (int)codepoint) == NULL;
}

static bool utf8_to_utf16(const void* name, size_t name_size, uint16_t output[FILESYSTEM_NAME_MAX], size_t* out_count) {
	const uint8_t* bytes = name;
	size_t         input = 0u;
	size_t         count = 0u;

	if (name == NULL || name_size == 0u || name_size > FILESYSTEM_NAME_MAX || out_count == NULL ||
	    bytes[name_size - 1u] == ' ' || bytes[name_size - 1u] == '.')
		return false;
	while (input < name_size) {
		uint32_t codepoint;
		size_t   continuation;
		uint8_t  first = bytes[input++];
		if (first < 0x80u) {
			codepoint    = first;
			continuation = 0u;
		}
		else if (first >= 0xc2u && first <= 0xdfu) {
			codepoint    = first & 0x1fu;
			continuation = 1u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			codepoint    = first & 0x0fu;
			continuation = 2u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			codepoint    = first & 0x07u;
			continuation = 3u;
		}
		else return false;
		if (continuation > name_size - input) return false;
		for (size_t index = 0u; index < continuation; index++) {
			uint8_t next = bytes[input++];
			if ((next & 0xc0u) != 0x80u) return false;
			codepoint = codepoint << 6u | (next & 0x3fu);
		}
		if ((continuation == 2u && codepoint < 0x800u) || (continuation == 3u && codepoint < 0x10000u) ||
		    codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
		    !fat_name_codepoint_valid(codepoint))
			return false;
		if (codepoint < 0x10000u) {
			if (count == FILESYSTEM_NAME_MAX) return false;
			output[count++] = (uint16_t)codepoint;
		}
		else {
			if (count > FILESYSTEM_NAME_MAX - 2u) return false;
			codepoint -= 0x10000u;
			output[count++] = (uint16_t)(0xd800u | codepoint >> 10u);
			output[count++] = (uint16_t)(0xdc00u | (codepoint & 0x3ffu));
		}
	}
	*out_count = count;
	return count != 0u;
}

static bool short_name_character(char value) {
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
	       value == '_' || value == '-' || value == '$' || value == '~' || value == '!' || value == '#' ||
	       value == '%' || value == '&' || value == '\'' || value == '(' || value == ')' || value == '@' ||
	       value == '^' || value == '`' || value == '{' || value == '}';
}

static bool encode_short_name(const char* name, uint8_t output[11]) {
	if (name == NULL) return false;
	memset(output, ' ', 11u);
	size_t base = 0u;
	while (name[base] != '\0' && name[base] != '.') {
		if (base == 8u || !short_name_character(name[base])) return false;
		char value   = name[base];
		output[base] = (uint8_t)(value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value);
		base++;
	}
	if (base == 0u) return false;
	if (name[base] == '\0') return true;
	size_t extension = 0u;
	for (size_t index = base + 1u; name[index] != '\0'; index++) {
		if (extension == 3u || !short_name_character(name[index])) return false;
		char value               = name[index];
		output[8u + extension++] = (uint8_t)(value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value);
	}
	return extension != 0u;
}

static enum fat32_result find_directory_entry(struct fat32_volume* volume, uint32_t directory_cluster,
                                              const uint8_t name[11], struct root_entry* out) {
	*out                    = (struct root_entry){0};
	uint32_t cluster        = directory_cluster;
	size_t   slots_seen     = 0u;
	bool     have_free_slot = false;

	for (;;) {
		out->tail_cluster     = cluster;
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* entry = volume->buffer.address + offset;
				if (++slots_seen > FAT32_DIRECTORY_MAX_SLOTS) return FAT32_RESULT_UNSUPPORTED;
				if (entry[0u] == 0u) {
					if (!have_free_slot) {
						out->sector = first_sector + sector;
						out->offset = offset;
					}
					out->slot_available = true;
					return FAT32_RESULT_OK;
				}
				if (entry[0u] == 0xe5u) {
					if (!have_free_slot) {
						out->sector    = first_sector + sector;
						out->offset    = offset;
						have_free_slot = true;
					}
					continue;
				}
				if (entry[11u] == FAT32_ATTRIBUTE_LONG_NAME || (entry[11u] & FAT32_ATTRIBUTE_VOLUME_ID) != 0u ||
				    memcmp(entry, name, 11u) != 0)
					continue;
				out->sector         = first_sector + sector;
				out->offset         = offset;
				out->cluster        = ((uint32_t)read_u16_le(entry + 20u) << 16u) | read_u16_le(entry + 26u);
				out->size           = read_u32_le(entry + 28u);
				out->attributes     = entry[11u];
				out->found          = true;
				out->slot_available = true;
				return FAT32_RESULT_OK;
			}
		}
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) return result;
		if (end) {
			out->slot_available = have_free_slot;
			return FAT32_RESULT_OK;
		}
		cluster = next;
	}
}

static enum fat32_result collect_chain(struct fat32_volume* volume, uint32_t first, uint32_t size,
                                       uint32_t** out_clusters, size_t* out_count) {
	uint64_t cluster_size = (uint64_t)volume->device.block_size * volume->sectors_per_cluster;
	size_t   count        = size == 0u ? 0u : (size_t)((size + cluster_size - 1u) / cluster_size);
	*out_clusters         = NULL;
	*out_count            = 0u;
	if (size > FAT32_WRITE_MAX_FILE_SIZE || (count == 0u && first != 0u) ||
	    (count != 0u && !fat32_volume_cluster_valid(volume, first)))
		return FAT32_RESULT_UNSUPPORTED;
	if (count == 0u) return FAT32_RESULT_OK;
	uint32_t* clusters = malloc(count * sizeof(*clusters));
	if (clusters == NULL) return FAT32_RESULT_ERROR;
	uint32_t current = first;
	for (size_t index = 0u; index < count; index++) {
		for (size_t prior = 0u; prior < index; prior++)
			if (clusters[prior] == current) {
				free(clusters);
				return FAT32_RESULT_UNSUPPORTED;
			}
		clusters[index] = current;
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, current, &next, &end);
		if (result != FAT32_RESULT_OK || (index + 1u == count) != end) {
			free(clusters);
			return result == FAT32_RESULT_OK ? FAT32_RESULT_UNSUPPORTED : result;
		}
		current = next;
	}
	*out_clusters = clusters;
	*out_count    = count;
	return FAT32_RESULT_OK;
}

static enum fat32_result allocate_cluster(struct fat32_volume* volume, uint32_t* out_cluster) {
	uint32_t start = fat32_volume_cluster_valid(volume, volume->allocation_hint) ? volume->allocation_hint : 2u;
	for (uint32_t offset = 0u; offset < volume->cluster_count; offset++) {
		uint32_t          candidate = 2u + (uint32_t)(((uint64_t)(start - 2u) + offset) % volume->cluster_count);
		uint32_t          value;
		enum fat32_result result = fat32_volume_read_fat(volume, candidate, &value);
		if (result != FAT32_RESULT_OK) return result;
		if (value != 0u) continue;
		result = fat32_volume_write_fat(volume, candidate, FAT32_END_OF_CHAIN);
		if (result != FAT32_RESULT_OK) return result;
		result = fat32_volume_zero_cluster(volume, candidate);
		if (result != FAT32_RESULT_OK) {
			(void)fat32_volume_write_fat(volume, candidate, 0u);
			return result;
		}
		volume->allocation_hint = candidate == volume->cluster_count + 1u ? 2u : candidate + 1u;
		*out_cluster            = candidate;
		return FAT32_RESULT_OK;
	}
	return FAT32_RESULT_UNSUPPORTED;
}

static enum fat32_result release_clusters(struct fat32_volume* volume, const uint32_t* clusters, size_t count) {
	for (size_t index = 0u; index < count; index++) {
		enum fat32_result result = fat32_volume_write_fat(volume, clusters[index], 0u);
		if (result != FAT32_RESULT_OK) return result;
	}
	return FAT32_RESULT_OK;
}

static enum fat32_result allocate_chain(struct fat32_volume* volume, size_t size, uint32_t** out_clusters,
                                        size_t* out_count) {
	uint64_t cluster_size = (uint64_t)volume->device.block_size * volume->sectors_per_cluster;
	size_t   count        = size == 0u ? 0u : (size_t)((size + cluster_size - 1u) / cluster_size);
	*out_clusters         = NULL;
	*out_count            = 0u;
	if (size > FAT32_WRITE_MAX_FILE_SIZE || count > volume->cluster_count) return FAT32_RESULT_UNSUPPORTED;
	if (count == 0u) return FAT32_RESULT_OK;
	uint32_t* clusters = malloc(count * sizeof(*clusters));
	if (clusters == NULL) return FAT32_RESULT_ERROR;
	for (size_t index = 0u; index < count; index++) {
		enum fat32_result result = allocate_cluster(volume, &clusters[index]);
		if (result != FAT32_RESULT_OK) {
			(void)release_clusters(volume, clusters, index);
			free(clusters);
			return result;
		}
	}
	for (size_t index = 0u; index + 1u < count; index++) {
		enum fat32_result result = fat32_volume_write_fat(volume, clusters[index], clusters[index + 1u]);
		if (result != FAT32_RESULT_OK) {
			(void)release_clusters(volume, clusters, count);
			free(clusters);
			return result;
		}
	}
	*out_clusters = clusters;
	*out_count    = count;
	return FAT32_RESULT_OK;
}

static enum fat32_result write_chain(struct fat32_volume* volume, const uint32_t* clusters, size_t count,
                                     const uint8_t* data, size_t size) {
	size_t position = 0u;
	for (size_t index = 0u; index < count; index++) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, clusters[index]);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			memset(volume->buffer.address, 0, volume->device.block_size);
			size_t chunk = size - position;
			if (chunk > volume->device.block_size) chunk = volume->device.block_size;
			if (chunk != 0u) memcpy(volume->buffer.address, data + position, chunk);
			enum fat32_result result = fat32_volume_write_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			position += chunk;
		}
	}
	return position == size ? FAT32_RESULT_OK : FAT32_RESULT_UNSUPPORTED;
}

static enum fat32_result extend_directory(struct fat32_volume* volume, struct root_entry* entry) {
	uint32_t          cluster;
	enum fat32_result result = allocate_cluster(volume, &cluster);
	if (result != FAT32_RESULT_OK) return result;
	result = fat32_volume_write_fat(volume, entry->tail_cluster, cluster);
	if (result != FAT32_RESULT_OK) {
		(void)fat32_volume_write_fat(volume, cluster, 0u);
		return result;
	}
	entry->sector         = fat32_volume_cluster_sector(volume, cluster);
	entry->offset         = 0u;
	entry->slot_available = true;
	return FAT32_RESULT_OK;
}

static bool short_name_canonical(const void* name, size_t name_size, const uint8_t encoded[11]) {
	char   decoded[FAT32_SHORT_NAME_CAPACITY];
	size_t base_size      = 8u;
	size_t extension_size = 3u;
	size_t position       = 0u;
	while (base_size != 0u && encoded[base_size - 1u] == ' ') base_size--;
	while (extension_size != 0u && encoded[8u + extension_size - 1u] == ' ') extension_size--;
	for (size_t index = 0u; index < base_size; index++) decoded[position++] = (char)encoded[index];
	if (extension_size != 0u) {
		decoded[position++] = '.';
		for (size_t index = 0u; index < extension_size; index++) decoded[position++] = (char)encoded[8u + index];
	}
	return position == name_size && memcmp(decoded, name, name_size) == 0;
}

static bool alias_character(uint8_t value) {
	return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
}

static enum fat32_result choose_short_name(struct fat32_volume* volume, uint32_t directory_cluster, const void* name,
                                           size_t name_size, uint8_t output[11], bool* out_long_name) {
	const uint8_t*    bytes = name;
	char              terminated[FILESYSTEM_NAME_MAX + 1u];
	struct root_entry existing;
	bool              encoded;

	memcpy(terminated, name, name_size);
	terminated[name_size] = '\0';
	encoded               = encode_short_name(terminated, output);
	if (encoded) {
		enum fat32_result result = find_directory_entry(volume, directory_cluster, output, &existing);
		if (result != FAT32_RESULT_OK) return result;
		if (!existing.found) {
			*out_long_name = !short_name_canonical(name, name_size, output);
			return FAT32_RESULT_OK;
		}
	}

	uint8_t stem[8u];
	uint8_t extension[3u];
	size_t  stem_size      = 0u;
	size_t  extension_size = 0u;
	size_t  dot            = name_size;
	for (size_t index = name_size; index != 0u; index--)
		if (bytes[index - 1u] == '.') {
			dot = index - 1u;
			break;
		}
	for (size_t index = 0u; index < dot && stem_size < sizeof(stem); index++) {
		uint8_t value = bytes[index];
		if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
		if (alias_character(value)) stem[stem_size++] = value;
	}
	if (stem_size == 0u) {
		memcpy(stem, "DIRECT", 6u);
		stem_size = 6u;
	}
	if (dot != name_size) {
		for (size_t index = dot + 1u; index < name_size && extension_size < sizeof(extension); index++) {
			uint8_t value = bytes[index];
			if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
			if (alias_character(value)) extension[extension_size++] = value;
		}
	}
	for (uint32_t sequence = 1u; sequence <= 999999u; sequence++) {
		uint8_t  digits[6u];
		size_t   digit_count = 0u;
		uint32_t value       = sequence;
		do {
			digits[digit_count++] = (uint8_t)('0' + value % 10u);
			value /= 10u;
		} while (value != 0u);
		size_t prefix = 8u - digit_count - 1u;
		if (prefix > stem_size) prefix = stem_size;
		memset(output, ' ', 11u);
		memcpy(output, stem, prefix);
		output[prefix] = '~';
		for (size_t index = 0u; index < digit_count; index++)
			output[prefix + 1u + index] = digits[digit_count - 1u - index];
		memcpy(output + 8u, extension, extension_size);
		enum fat32_result result = find_directory_entry(volume, directory_cluster, output, &existing);
		if (result != FAT32_RESULT_OK) return result;
		if (!existing.found) {
			*out_long_name = true;
			return FAT32_RESULT_OK;
		}
	}
	return FAT32_RESULT_UNSUPPORTED;
}

static enum fat32_result find_free_slots(struct fat32_volume* volume, uint32_t directory_cluster, size_t needed,
                                         struct directory_slot slots[FAT32_LONG_NAME_MAXIMUM_SLOTS + 1u]) {
	uint32_t cluster = directory_cluster;
	size_t   run     = 0u;
	bool     ended   = false;

	for (;;) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* entry = volume->buffer.address + offset;
				if (entry[0u] == 0u) ended = true;
				if (ended || entry[0u] == 0xe5u) {
					slots[run++] = (struct directory_slot){.sector = first_sector + sector, .offset = offset};
					if (run == needed) return FAT32_RESULT_OK;
				}
				else run = 0u;
			}
		}
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) return result;
		if (end) {
			struct root_entry tail = {.tail_cluster = cluster};
			result                 = extend_directory(volume, &tail);
			if (result != FAT32_RESULT_OK) return result;
			next  = (uint32_t)((tail.sector - volume->data_start) / volume->sectors_per_cluster) + 2u;
			ended = true;
		}
		cluster = next;
	}
}

static void format_long_name_entry(uint8_t entry[FAT32_DIRECTORY_ENTRY_SIZE], uint8_t order, uint8_t checksum,
                                   const uint16_t units[FILESYSTEM_NAME_MAX], size_t unit_count) {
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
	uint8_t index = order & 0x1fu;
	memset(entry, 0xff, FAT32_DIRECTORY_ENTRY_SIZE);
	entry[0u]  = order;
	entry[11u] = FAT32_ATTRIBUTE_LONG_NAME;
	entry[12u] = 0u;
	entry[13u] = checksum;
	entry[26u] = 0u;
	entry[27u] = 0u;
	for (size_t position = 0u; position < FAT32_LONG_NAME_CHARACTERS_PER_SLOT; position++) {
		size_t   unit_index = (size_t)(index - 1u) * FAT32_LONG_NAME_CHARACTERS_PER_SLOT + position;
		uint16_t unit       = unit_index < unit_count ? units[unit_index] : unit_index == unit_count ? 0u : 0xffffu;
		write_u16_le(entry + offsets[position], unit);
	}
}

static void format_directory_entry(uint8_t entry[FAT32_DIRECTORY_ENTRY_SIZE], const uint8_t name[11],
                                   uint8_t attributes, uint32_t cluster, uint32_t size) {
	memset(entry, 0, FAT32_DIRECTORY_ENTRY_SIZE);
	memcpy(entry, name, 11u);
	entry[11u] = attributes;
	write_u16_le(entry + 16u, 0x0021u);
	write_u16_le(entry + 18u, 0x0021u);
	write_u16_le(entry + 20u, (uint16_t)(cluster >> 16u));
	write_u16_le(entry + 24u, 0x0021u);
	write_u16_le(entry + 26u, (uint16_t)cluster);
	write_u32_le(entry + 28u, size);
}

static enum fat32_result write_directory_entry(struct fat32_volume* volume, const struct root_entry* location,
                                               const uint8_t name[11], uint8_t attributes, uint32_t cluster,
                                               uint32_t size) {
	enum fat32_result result = fat32_volume_read_sector(volume, location->sector);
	if (result != FAT32_RESULT_OK) return result;
	uint8_t* entry = volume->buffer.address + location->offset;
	format_directory_entry(entry, name, attributes, cluster, size);
	return fat32_volume_write_sector(volume, location->sector);
}

static enum fat32_result write_slot(struct fat32_volume* volume, struct directory_slot slot,
                                    const uint8_t entry[FAT32_DIRECTORY_ENTRY_SIZE]) {
	enum fat32_result result = fat32_volume_read_sector(volume, slot.sector);
	if (result != FAT32_RESULT_OK) return result;
	memcpy(volume->buffer.address + slot.offset, entry, FAT32_DIRECTORY_ENTRY_SIZE);
	return fat32_volume_write_sector(volume, slot.sector);
}

static void delete_slots_best_effort(struct fat32_volume* volume, const struct directory_slot* slots, size_t count) {
	for (size_t index = 0u; index < count; index++) {
		if (fat32_volume_read_sector(volume, slots[index].sector) != FAT32_RESULT_OK) continue;
		volume->buffer.address[slots[index].offset] = 0xe5u;
		(void)fat32_volume_write_sector(volume, slots[index].sector);
	}
}

static enum fat32_result create_named_entry(struct fat32_volume* volume, uint32_t directory_cluster, const void* name,
                                            size_t name_size, uint8_t attributes, uint32_t node_cluster,
                                            uint32_t node_size, struct directory_slot* out_slots,
                                            size_t* out_slot_count) {
	uint16_t units[FILESYSTEM_NAME_MAX];
	uint8_t  short_name[11u];
	size_t   unit_count;
	bool     long_name;

	if (!utf8_to_utf16(name, name_size, units, &unit_count)) return FAT32_RESULT_UNSUPPORTED;
	enum fat32_result result = choose_short_name(volume, directory_cluster, name, name_size, short_name, &long_name);
	if (result != FAT32_RESULT_OK) return result;
	size_t long_count =
		long_name ? (unit_count + 1u + FAT32_LONG_NAME_CHARACTERS_PER_SLOT - 1u) / FAT32_LONG_NAME_CHARACTERS_PER_SLOT
		          : 0u;
	if (long_count > FAT32_LONG_NAME_MAXIMUM_SLOTS) return FAT32_RESULT_UNSUPPORTED;
	struct directory_slot  local_slots[FAT32_LONG_NAME_MAXIMUM_SLOTS + 1u];
	struct directory_slot* slots      = out_slots != NULL ? out_slots : local_slots;
	size_t                 slot_count = long_count + 1u;
	result                            = find_free_slots(volume, directory_cluster, slot_count, slots);
	if (result != FAT32_RESULT_OK) return result;

	uint8_t raw[FAT32_DIRECTORY_ENTRY_SIZE];
	uint8_t checksum = short_name_checksum(short_name);
	for (size_t index = 0u; index < long_count; index++) {
		uint8_t order = (uint8_t)(long_count - index);
		if (index == 0u) order |= FAT32_LONG_NAME_LAST;
		format_long_name_entry(raw, order, checksum, units, unit_count);
		result = write_slot(volume, slots[index], raw);
		if (result != FAT32_RESULT_OK) {
			delete_slots_best_effort(volume, slots, index);
			return result;
		}
	}
	format_directory_entry(raw, short_name, attributes, node_cluster, node_size);
	result = write_slot(volume, slots[long_count], raw);
	if (result != FAT32_RESULT_OK) {
		delete_slots_best_effort(volume, slots, long_count);
		return result;
	}
	if (out_slot_count != NULL) *out_slot_count = slot_count;
	return FAT32_RESULT_OK;
}

static enum fat32_result locate_entry_slots(struct fat32_volume* volume, uint32_t directory_cluster,
                                            const uint8_t short_name[11u], struct directory_slot* out_slots,
                                            size_t* out_count) {
	struct directory_slot pending[FAT32_LONG_NAME_MAXIMUM_SLOTS];
	size_t                pending_count = 0u;
	uint32_t              cluster       = directory_cluster;

	for (;;) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* raw = volume->buffer.address + offset;
				if (raw[0u] == 0u) return FAT32_RESULT_NOT_FOUND;
				if (raw[0u] == 0xe5u) {
					pending_count = 0u;
					continue;
				}
				if (raw[11u] == FAT32_ATTRIBUTE_LONG_NAME) {
					if (pending_count == FAT32_LONG_NAME_MAXIMUM_SLOTS) pending_count = 0u;
					pending[pending_count++] =
						(struct directory_slot){.sector = first_sector + sector, .offset = offset};
					continue;
				}
				if (memcmp(raw, short_name, 11u) == 0) {
					for (size_t index = 0u; index < pending_count; index++) out_slots[index] = pending[index];
					out_slots[pending_count] =
						(struct directory_slot){.sector = first_sector + sector, .offset = offset};
					*out_count = pending_count + 1u;
					return FAT32_RESULT_OK;
				}
				pending_count = 0u;
			}
		}
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) return result;
		if (end) return FAT32_RESULT_NOT_FOUND;
		cluster = next;
	}
}

static enum fat32_result delete_entry_slots(struct fat32_volume* volume, uint32_t directory_cluster,
                                            const uint8_t short_name[11u]) {
	struct directory_slot slots[FAT32_LONG_NAME_MAXIMUM_SLOTS + 1u];
	size_t                count;
	enum fat32_result     result = locate_entry_slots(volume, directory_cluster, short_name, slots, &count);
	if (result != FAT32_RESULT_OK) return result;
	for (size_t index = 0u; index < count;) {
		uint64_t sector = slots[index].sector;
		result          = fat32_volume_read_sector(volume, sector);
		if (result != FAT32_RESULT_OK) return result;
		do {
			volume->buffer.address[slots[index].offset] = 0xe5u;
			index++;
		} while (index < count && slots[index].sector == sector);
		result = fat32_volume_write_sector(volume, sector);
		if (result != FAT32_RESULT_OK) return result;
	}
	return FAT32_RESULT_OK;
}

static enum fat32_result verify_file(struct fat32_volume* volume, const uint8_t name[11], const uint8_t* data,
                                     size_t size) {
	struct root_entry entry;
	enum fat32_result result = find_directory_entry(volume, volume->root_cluster, name, &entry);
	if (result != FAT32_RESULT_OK || !entry.found || entry.size != size) return FAT32_RESULT_UNSUPPORTED;
	uint32_t* clusters;
	size_t    count;
	result = collect_chain(volume, entry.cluster, entry.size, &clusters, &count);
	if (result != FAT32_RESULT_OK) return result;
	size_t position = 0u;
	for (size_t index = 0u; index < count && result == FAT32_RESULT_OK; index++) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, clusters[index]);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster && position < size; sector++) {
			result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) break;
			size_t chunk = size - position;
			if (chunk > volume->device.block_size) chunk = volume->device.block_size;
			if (memcmp(volume->buffer.address, data + position, chunk) != 0) result = FAT32_RESULT_UNSUPPORTED;
			position += chunk;
		}
	}
	free(clusters);
	return result == FAT32_RESULT_OK && position == size ? FAT32_RESULT_OK : FAT32_RESULT_UNSUPPORTED;
}

enum fat32_result fat32_write_root_file(struct fat32_volume* volume, const char* name, const void* data, size_t size) {
	if (volume == NULL || (data == NULL && size != 0u) || size > UINT32_MAX ||
	    (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	uint8_t encoded_name[11];
	if (!encode_short_name(name, encoded_name)) return FAT32_RESULT_UNSUPPORTED;
	struct root_entry location;
	enum fat32_result result = find_directory_entry(volume, volume->root_cluster, encoded_name, &location);
	if (result != FAT32_RESULT_OK) return result;
	uint32_t* old_clusters = NULL;
	size_t    old_count    = 0u;
	if (location.found) {
		if ((location.attributes & (FAT32_ATTRIBUTE_READ_ONLY | FAT32_ATTRIBUTE_DIRECTORY)) != 0u)
			return FAT32_RESULT_UNSUPPORTED;
		result = collect_chain(volume, location.cluster, location.size, &old_clusters, &old_count);
		if (result != FAT32_RESULT_OK) return result;
	}
	uint32_t* new_clusters;
	size_t    new_count;
	result = allocate_chain(volume, size, &new_clusters, &new_count);
	if (result != FAT32_RESULT_OK) goto done;
	result = write_chain(volume, new_clusters, new_count, data, size);
	if (result != FAT32_RESULT_OK) goto rollback;
	if (!location.slot_available) {
		result = extend_directory(volume, &location);
		if (result != FAT32_RESULT_OK) goto rollback;
	}
	result = write_directory_entry(volume,
	                               &location,
	                               encoded_name,
	                               FAT32_ATTRIBUTE_ARCHIVE,
	                               new_count == 0u ? 0u : new_clusters[0u],
	                               (uint32_t)size);
	if (result != FAT32_RESULT_OK) goto uncertain_commit;
	result = release_clusters(volume, old_clusters, old_count);
	if (result != FAT32_RESULT_OK) goto committed;
	result = fat32_volume_invalidate_fsinfo(volume);

committed:
	if (result == FAT32_RESULT_OK) result = fat32_volume_flush(volume);
	if (result == FAT32_RESULT_OK) result = verify_file(volume, encoded_name, data, size);
	free(new_clusters);
	goto done;

rollback:
	(void)release_clusters(volume, new_clusters, new_count);
	(void)fat32_volume_invalidate_fsinfo(volume);
	free(new_clusters);
	goto done;

uncertain_commit:
	(void)fat32_volume_invalidate_fsinfo(volume);
	free(new_clusters);
done:
	free(old_clusters);
	return result;
}

static enum fat32_result initialize_directory(struct fat32_volume* volume, uint32_t cluster, uint32_t parent_cluster) {
	static const uint8_t dot_name[11]    = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
	static const uint8_t dotdot_name[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
	uint64_t             first_sector    = fat32_volume_cluster_sector(volume, cluster);
	enum fat32_result    result;

	result = fat32_volume_read_sector(volume, first_sector);
	if (result != FAT32_RESULT_OK) return result;
	memset(volume->buffer.address, 0, volume->device.block_size);
	format_directory_entry(volume->buffer.address, dot_name, FAT32_ATTRIBUTE_DIRECTORY, cluster, 0u);
	format_directory_entry(volume->buffer.address + FAT32_DIRECTORY_ENTRY_SIZE,
	                       dotdot_name,
	                       FAT32_ATTRIBUTE_DIRECTORY,
	                       parent_cluster,
	                       0u);
	return fat32_volume_write_sector(volume, first_sector);
}

enum fat32_result fat32_create_directory(struct fat32_volume* volume, uint32_t parent_cluster, const void* name,
                                         size_t name_size) {
	uint32_t          cluster;
	enum fat32_result result;

	if (volume == NULL || !fat32_volume_cluster_valid(volume, parent_cluster) ||
	    (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	result = allocate_cluster(volume, &cluster);
	if (result != FAT32_RESULT_OK) return result;
	result = initialize_directory(volume, cluster, parent_cluster == volume->root_cluster ? 0u : parent_cluster);
	if (result != FAT32_RESULT_OK) goto rollback;
	result =
		create_named_entry(volume, parent_cluster, name, name_size, FAT32_ATTRIBUTE_DIRECTORY, cluster, 0u, NULL, NULL);
	if (result != FAT32_RESULT_OK) {
		struct fat32_directory_entry committed;
		enum fat32_result lookup = fat32_directory_lookup(volume, parent_cluster, name, name_size, &committed);
		if (lookup != FAT32_RESULT_OK || committed.cluster != cluster) goto rollback;
		goto uncertain_commit;
	}
	result = fat32_volume_invalidate_fsinfo(volume);
	if (result == FAT32_RESULT_OK) result = fat32_volume_flush(volume);
	if (result == FAT32_RESULT_OK) {
		struct fat32_directory_entry entry;
		result = fat32_directory_lookup(volume, parent_cluster, name, name_size, &entry);
		if (result == FAT32_RESULT_OK &&
		    ((entry.attributes & FAT32_ATTRIBUTE_DIRECTORY) == 0u || entry.cluster != cluster))
			result = FAT32_RESULT_UNSUPPORTED;
	}
	return result;

rollback:
	(void)fat32_volume_write_fat(volume, cluster, 0u);
uncertain_commit:
	(void)fat32_volume_invalidate_fsinfo(volume);
	return result;
}

static enum fat32_result release_chain(struct fat32_volume* volume, uint32_t first_cluster) {
	uint32_t* clusters = NULL;
	size_t    count    = 0u;
	size_t    capacity = 0u;
	uint32_t  cluster  = first_cluster;
	bool      complete = false;
	while (count < volume->cluster_count) {
		if (count == capacity) {
			size_t next_capacity = capacity == 0u ? 64u : capacity * 2u;
			if (next_capacity > volume->cluster_count) next_capacity = volume->cluster_count;
			uint32_t* grown = realloc(clusters, next_capacity * sizeof(*clusters));
			if (grown == NULL) {
				free(clusters);
				return FAT32_RESULT_ERROR;
			}
			clusters = grown;
			capacity = next_capacity;
		}
		clusters[count++] = cluster;
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) {
			free(clusters);
			return result;
		}
		if (end) {
			complete = true;
			break;
		}
		cluster = next;
	}
	if (!complete) {
		free(clusters);
		return FAT32_RESULT_UNSUPPORTED;
	}
	qsort(clusters, count, sizeof(*clusters), compare_clusters);
	enum fat32_result result = fat32_volume_clear_clusters(volume, clusters, count);
	free(clusters);
	return result;
}

enum fat32_result fat32_remove_node(struct fat32_volume* volume, uint32_t parent_cluster, const void* name,
                                    size_t name_size, bool defer_sync) {
	struct fat32_directory_entry entry;
	enum fat32_result            result;

	if (volume == NULL || !fat32_volume_cluster_valid(volume, parent_cluster) ||
	    (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	result = fat32_directory_lookup(volume, parent_cluster, name, name_size, &entry);
	if (result != FAT32_RESULT_OK) return result;
	if ((entry.attributes & FAT32_ATTRIBUTE_READ_ONLY) != 0u) return FAT32_RESULT_UNSUPPORTED;
	if ((entry.attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u) {
		struct fat32_directory_entry child;
		size_t                       returned;
		uint64_t                     total;
		result = fat32_directory_enumerate(volume, entry.cluster, 0u, &child, 1u, &returned, &total);
		if (result != FAT32_RESULT_OK) return result;
		if (total != 0u) return FAT32_RESULT_NOT_EMPTY;
	}
	result = delete_entry_slots(volume, parent_cluster, entry.short_name);
	if (result != FAT32_RESULT_OK) return result;
	if (entry.cluster != 0u) {
		result = release_chain(volume, entry.cluster);
		if (result != FAT32_RESULT_OK) return result;
	}
	fat32_volume_mark_fsinfo_dirty(volume);
	return defer_sync ? FAT32_RESULT_OK : fat32_volume_sync(volume);
}

static enum fat32_result update_parent_entry(struct fat32_volume* volume, uint32_t directory_cluster,
                                             uint32_t parent_cluster) {
	uint64_t          sector = fat32_volume_cluster_sector(volume, directory_cluster);
	enum fat32_result result = fat32_volume_read_sector(volume, sector);
	if (result != FAT32_RESULT_OK) return result;
	uint8_t* entry = volume->buffer.address + FAT32_DIRECTORY_ENTRY_SIZE;
	if (entry[0u] != '.' || entry[1u] != '.' || (entry[11u] & FAT32_ATTRIBUTE_DIRECTORY) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	uint32_t encoded = parent_cluster == volume->root_cluster ? 0u : parent_cluster;
	write_u16_le(entry + 20u, (uint16_t)(encoded >> 16u));
	write_u16_le(entry + 26u, (uint16_t)encoded);
	return fat32_volume_write_sector(volume, sector);
}

enum fat32_result fat32_rename_node(struct fat32_volume* volume, uint32_t old_parent_cluster, const void* old_name,
                                    size_t old_name_size, uint32_t new_parent_cluster, const void* new_name,
                                    size_t new_name_size) {
	struct fat32_directory_entry entry;
	struct directory_slot        new_slots[FAT32_LONG_NAME_MAXIMUM_SLOTS + 1u];
	size_t                       new_slot_count = 0u;
	enum fat32_result            result;

	if (volume == NULL || !fat32_volume_cluster_valid(volume, old_parent_cluster) ||
	    !fat32_volume_cluster_valid(volume, new_parent_cluster) || (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	result = fat32_directory_lookup(volume, old_parent_cluster, old_name, old_name_size, &entry);
	if (result != FAT32_RESULT_OK) return result;
	if ((entry.attributes & FAT32_ATTRIBUTE_READ_ONLY) != 0u) return FAT32_RESULT_UNSUPPORTED;
	struct fat32_directory_entry existing;
	result = fat32_directory_lookup(volume, new_parent_cluster, new_name, new_name_size, &existing);
	if (result == FAT32_RESULT_OK) {
		if (new_parent_cluster != old_parent_cluster ||
		    memcmp(existing.short_name, entry.short_name, sizeof(entry.short_name)) != 0)
			return FAT32_RESULT_ALREADY_EXISTS;
	}
	else if (result != FAT32_RESULT_NOT_FOUND) return result;
	result = create_named_entry(volume,
	                            new_parent_cluster,
	                            new_name,
	                            new_name_size,
	                            entry.attributes,
	                            entry.cluster,
	                            entry.size,
	                            new_slots,
	                            &new_slot_count);
	if (result != FAT32_RESULT_OK) return result;
	bool moved_directory =
		(entry.attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u && old_parent_cluster != new_parent_cluster;
	if (moved_directory) {
		result = update_parent_entry(volume, entry.cluster, new_parent_cluster);
		if (result != FAT32_RESULT_OK) goto rollback;
	}
	result = delete_entry_slots(volume, old_parent_cluster, entry.short_name);
	if (result != FAT32_RESULT_OK) {
		if (moved_directory) (void)update_parent_entry(volume, entry.cluster, old_parent_cluster);
		goto rollback;
	}
	result = fat32_volume_flush(volume);
	return result;

rollback:
	delete_slots_best_effort(volume, new_slots, new_slot_count);
	return result;
}
