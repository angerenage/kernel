#pragma once

#include <protocol/filesystem.h>
#include <stddef.h>
#include <stdint.h>

#include "volume.h"

#define FAT32_SHORT_NAME_CAPACITY 13u

struct fat32_directory_entry {
	char     name[FILESYSTEM_NAME_MAX + 1u];
	uint32_t cluster;
	uint32_t size;
	uint8_t  attributes;
};

/* Look up one FAT name with ASCII case folding in a directory cluster chain. */
enum fat32_result fat32_directory_lookup(struct fat32_volume* volume, uint32_t directory_cluster, const void* name,
                                         size_t name_size, struct fat32_directory_entry* out_entry);

/* Read one page of live entries and return the directory's total entry count. */
enum fat32_result fat32_directory_enumerate(struct fat32_volume* volume, uint32_t directory_cluster, uint64_t offset,
                                            struct fat32_directory_entry* entries, size_t capacity,
                                            size_t* out_returned, uint64_t* out_total);

/* Read bytes from a validated regular-file entry. */
enum fat32_result fat32_file_read(struct fat32_volume* volume, const struct fat32_directory_entry* entry,
                                  uint64_t offset, void* buffer, size_t size, size_t* out_read);
