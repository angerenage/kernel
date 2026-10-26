#pragma once

#include <base/cap.h>

#include "volume.h"

/* Advertise and serve one mounted FAT32 volume until an unrecoverable error. */
enum fat32_result fat32_server_run(struct fat32_volume* volume, cap_id_t device_cap);
