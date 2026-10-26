#include "directory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

static void format_short_name(const uint8_t* entry, char output[13]) {
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

static enum fat32_result process_entry(const struct fat32_volume* volume, const uint8_t* entry, size_t* count) {
	uint8_t attributes = entry[11u];
	if (entry[0u] == 0xe5u || attributes == FAT32_ATTRIBUTE_LONG_NAME) return FAT32_RESULT_OK;
	if ((attributes & 0xc0u) != 0u) return FAT32_RESULT_UNSUPPORTED;
	if ((attributes & FAT32_ATTRIBUTE_VOLUME_ID) != 0u) return FAT32_RESULT_OK;
	uint32_t cluster   = ((uint32_t)read_u16_le(entry + 20u) << 16u) | read_u16_le(entry + 26u);
	uint32_t size      = read_u32_le(entry + 28u);
	bool     directory = (attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u;
	if ((cluster == 0u && (directory || size != 0u)) || (cluster != 0u && !fat32_volume_cluster_valid(volume, cluster)))
		return FAT32_RESULT_UNSUPPORTED;
	char name[13];
	format_short_name(entry, name);
	if (name[0u] == '\0') return FAT32_RESULT_UNSUPPORTED;
	if (directory) printf("fat32: root directory \"%s\" cluster=%u\n", name, cluster);
	else printf("fat32: root file \"%s\" size=%u cluster=%u\n", name, size, cluster);
	(*count)++;
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_list_root(struct fat32_volume* volume) {
	if (volume == NULL || volume->device.block_size % FAT32_DIRECTORY_ENTRY_SIZE != 0u) return FAT32_RESULT_UNSUPPORTED;
	uint32_t cluster         = volume->root_cluster;
	uint32_t slow            = cluster;
	uint32_t fast            = cluster;
	bool     checking_cycles = true;
	size_t   slots_seen      = 0u;
	size_t   entry_count     = 0u;

	for (;;) {
		uint64_t first_sector = fat32_volume_cluster_sector(volume, cluster);
		for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
			enum fat32_result result = fat32_volume_read_sector(volume, first_sector + sector);
			if (result != FAT32_RESULT_OK) return result;
			for (uint32_t offset = 0u; offset < volume->device.block_size; offset += FAT32_DIRECTORY_ENTRY_SIZE) {
				const uint8_t* entry = volume->buffer.address + offset;
				if (entry[0u] == 0u) {
					printf("fat32: root directory contains %zu live entr%s\n",
					       entry_count,
					       entry_count == 1u ? "y" : "ies");
					return FAT32_RESULT_OK;
				}
				if (++slots_seen > FAT32_DIRECTORY_MAX_SLOTS) return FAT32_RESULT_UNSUPPORTED;
				result = process_entry(volume, entry, &entry_count);
				if (result != FAT32_RESULT_OK) return result;
			}
		}
		uint32_t          next;
		bool              end;
		enum fat32_result result = fat32_volume_next_cluster(volume, cluster, &next, &end);
		if (result != FAT32_RESULT_OK) return result;
		if (end) {
			printf("fat32: root directory contains %zu live entr%s\n", entry_count, entry_count == 1u ? "y" : "ies");
			return FAT32_RESULT_OK;
		}
		if (checking_cycles) {
			uint32_t ignored;
			result = fat32_volume_next_cluster(volume, slow, &slow, &end);
			if (result != FAT32_RESULT_OK) return result;
			if (!end) {
				result = fat32_volume_next_cluster(volume, fast, &fast, &end);
				if (result != FAT32_RESULT_OK) return result;
			}
			if (!end) {
				result = fat32_volume_next_cluster(volume, fast, &ignored, &end);
				if (result != FAT32_RESULT_OK) return result;
				if (!end) fast = ignored;
			}
			if (end) checking_cycles = false;
			else if (slow == fast) return FAT32_RESULT_UNSUPPORTED;
		}
		cluster = next;
	}
}
