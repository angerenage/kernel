#pragma once

#include <stddef.h>

#include "volume.h"

/* Atomically replace one root-level 8.3 file as far as FAT32 permits. */
enum fat32_result fat32_write_root_file(struct fat32_volume* volume, const char* name, const void* data, size_t size);

/* Create one UTF-8 named directory in an existing directory cluster. */
enum fat32_result fat32_create_directory(struct fat32_volume* volume, uint32_t parent_cluster, const void* name,
                                         size_t name_size);

/* Remove a file or empty directory, including all of its long-name slots. */
enum fat32_result fat32_remove_node(struct fat32_volume* volume, uint32_t parent_cluster, const void* name,
                                    size_t name_size);

/* Rename or move a node between directories on this volume. */
enum fat32_result fat32_rename_node(struct fat32_volume* volume, uint32_t old_parent_cluster, const void* old_name,
                                    size_t old_name_size, uint32_t new_parent_cluster, const void* new_name,
                                    size_t new_name_size);
