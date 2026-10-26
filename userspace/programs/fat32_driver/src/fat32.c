#include "fat32.h"

#include <stddef.h>

#include "server.h"
#include "volume.h"

enum device_driver_result fat32_driver_run(cap_id_t device_cap) {
	struct fat32_volume volume;
	enum fat32_result   result = fat32_volume_open(device_cap, &volume);

	if (result == FAT32_RESULT_UNSUPPORTED) return DEVICE_DRIVER_UNSUPPORTED;
	if (result != FAT32_RESULT_OK) return DEVICE_DRIVER_ERROR;
	result = fat32_server_run(&volume, device_cap);
	fat32_volume_close(&volume);
	if (result == FAT32_RESULT_UNSUPPORTED) return DEVICE_DRIVER_UNSUPPORTED;
	if (result != FAT32_RESULT_OK) return DEVICE_DRIVER_ERROR;
	return DEVICE_DRIVER_ERROR;
}
