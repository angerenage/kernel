#include <stddef.h>
#include <stdio.h>

#include <protocol/device.h>
#include <system/capability.h>

#include "xhci.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
			capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID)
		return 1;
	cap_id_t device_cap = capv[DEVICE_DRIVER_CAPABILITY_DEVICE];
	enum device_driver_result result = xhci_driver_run(device_cap);
	(void)cap_drop(device_cap);
	if (result == DEVICE_DRIVER_UNSUPPORTED) return DEVICE_DRIVER_EXIT_UNSUPPORTED;
	fprintf(stderr, "xhci: driver stopped after an error\n");
	return 1;
}
