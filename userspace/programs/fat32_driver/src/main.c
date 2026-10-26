#include <protocol/device.h>
#include <stddef.h>
#include <system/capability.h>

#include "fat32.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	cap_id_t                  device_cap;
	enum device_driver_result result;

	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
	    capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID)
		return 1;
	device_cap = capv[DEVICE_DRIVER_CAPABILITY_DEVICE];
	result     = fat32_driver_run(device_cap);
	if (cap_drop(device_cap) != SYSCALL_STATUS_OK) result = DEVICE_DRIVER_ERROR;
	if (result == DEVICE_DRIVER_UNSUPPORTED) return DEVICE_DRIVER_EXIT_UNSUPPORTED;
	return 1;
}
