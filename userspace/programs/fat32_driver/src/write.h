#pragma once

#include <stddef.h>

#include "volume.h"

/* Atomically replace one root-level 8.3 file as far as FAT32 permits. */
enum fat32_result fat32_write_root_file(struct fat32_volume* volume, const char* name, const void* data, size_t size);
