#include "fat32.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/time.h>

#include "directory.h"
#include "volume.h"
#include "write.h"

enum device_driver_result fat32_driver_run(cap_id_t device_cap) {
	struct fat32_volume volume;
	enum fat32_result   result = fat32_volume_open(device_cap, &volume);

	if (result == FAT32_RESULT_UNSUPPORTED) return DEVICE_DRIVER_UNSUPPORTED;
	if (result != FAT32_RESULT_OK) return DEVICE_DRIVER_ERROR;
	if ((volume.device.flags & BLOCK_DEVICE_WRITABLE) != 0u) {
		uint8_t contents[1537];
		for (size_t index = 0u; index < sizeof(contents); index++)
			contents[index] = index % 64u == 63u ? '\n' : (uint8_t)('A' + index % 26u);
		result = fat32_write_root_file(&volume, "WRITE.TXT", contents, sizeof(contents));
		if (result == FAT32_RESULT_OK) printf("fat32: wrote and verified root file \"WRITE.TXT\"\n");
	}
	else printf("fat32: volume is read-only; skipping the write test\n");
	if (result == FAT32_RESULT_OK) result = fat32_list_root(&volume);
	fat32_volume_close(&volume);
	if (result == FAT32_RESULT_UNSUPPORTED) return DEVICE_DRIVER_UNSUPPORTED;
	if (result != FAT32_RESULT_OK) return DEVICE_DRIVER_ERROR;
	for (;;)
		if (sleep_ms(1000u) != SYSCALL_STATUS_OK) return DEVICE_DRIVER_ERROR;
}
