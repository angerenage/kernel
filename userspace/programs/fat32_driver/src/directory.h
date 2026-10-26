#pragma once

#include "volume.h"

/* Validate and print every live entry in the FAT32 root directory. */
enum fat32_result fat32_list_root(struct fat32_volume* volume);
