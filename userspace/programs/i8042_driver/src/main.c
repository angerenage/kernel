#include <protocol/device.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#include "i8042.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	cap_id_t device_cap;
	bool     success;

	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
	    capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID)
		return 1;
	device_cap = capv[DEVICE_DRIVER_CAPABILITY_DEVICE];
	success    = i8042_driver_run(device_cap);
	if (cap_drop(device_cap) != SYSCALL_STATUS_OK) success = false;
	if (!success) {
		static const char message[] = "i8042: driver stopped after an error\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
	}
	return success ? 0 : 1;
}
