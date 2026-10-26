#include "write.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FAT32_DIRECTORY_ENTRY_SIZE 32u
#define FAT32_DIRECTORY_MAX_SLOTS 65536u
#define FAT32_ATTRIBUTE_READ_ONLY 0x01u
#define FAT32_ATTRIBUTE_VOLUME_ID 0x08u
#define FAT32_ATTRIBUTE_DIRECTORY 0x10u
#define FAT32_ATTRIBUTE_LONG_NAME 0x0fu
#define FAT32_ATTRIBUTE_ARCHIVE 0x20u
#define FAT32_END_OF_CHAIN 0x0fffffffu
#define FAT32_WRITE_MAX_FILE_SIZE (16u * 1024u * 1024u)

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

static enum fat32_result find_root_entry(struct fat32_volume* volume, const uint8_t name[11], struct root_entry* out) {
	*out                    = (struct root_entry){0};
	uint32_t cluster        = volume->root_cluster;
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

static enum fat32_result extend_root(struct fat32_volume* volume, struct root_entry* entry) {
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

static enum fat32_result write_directory_entry(struct fat32_volume* volume, const struct root_entry* location,
                                               const uint8_t name[11], uint32_t cluster, uint32_t size) {
	enum fat32_result result = fat32_volume_read_sector(volume, location->sector);
	if (result != FAT32_RESULT_OK) return result;
	uint8_t* entry = volume->buffer.address + location->offset;
	memset(entry, 0, FAT32_DIRECTORY_ENTRY_SIZE);
	memcpy(entry, name, 11u);
	entry[11u] = FAT32_ATTRIBUTE_ARCHIVE;
	write_u16_le(entry + 16u, 0x0021u);
	write_u16_le(entry + 18u, 0x0021u);
	write_u16_le(entry + 20u, (uint16_t)(cluster >> 16u));
	write_u16_le(entry + 24u, 0x0021u);
	write_u16_le(entry + 26u, (uint16_t)cluster);
	write_u32_le(entry + 28u, size);
	return fat32_volume_write_sector(volume, location->sector);
}

static enum fat32_result verify_file(struct fat32_volume* volume, const uint8_t name[11], const uint8_t* data,
                                     size_t size) {
	struct root_entry entry;
	enum fat32_result result = find_root_entry(volume, name, &entry);
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
	enum fat32_result result = find_root_entry(volume, encoded_name, &location);
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
		result = extend_root(volume, &location);
		if (result != FAT32_RESULT_OK) goto rollback;
	}
	result =
		write_directory_entry(volume, &location, encoded_name, new_count == 0u ? 0u : new_clusters[0u], (uint32_t)size);
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
