#include <stddef.h>
#include <stdio.h>

#include <protocol/device.h>
#include <system/capability.h>
#include <runtime/log.h>

#include "bot.h"
#include "service.h"

static enum device_driver_result run(cap_id_t device_cap) {
	struct usb_msc_device device = {.transport_cap = CAP_ID_INVALID};
	if (!usb_msc_initialize(device_cap, &device)) return DEVICE_DRIVER_ERROR;
	RUNTIME_DEBUG_LOG("usb-storage: SCSI disk ready (%llu blocks, %u bytes/block)\n",
			(unsigned long long)device.block_count,
			device.block_size);
	bool success = usb_msc_block_service_run(device_cap, &device);
	usb_msc_deinitialize(&device);
	return success ? DEVICE_DRIVER_UNSUPPORTED : DEVICE_DRIVER_ERROR;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
			capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID)
		return 1;
	cap_id_t device_cap = capv[DEVICE_DRIVER_CAPABILITY_DEVICE];
	enum device_driver_result result = run(device_cap);
	if (cap_drop(device_cap) != SYSCALL_STATUS_OK) result = DEVICE_DRIVER_ERROR;
	if (result == DEVICE_DRIVER_UNSUPPORTED) return DEVICE_DRIVER_EXIT_UNSUPPORTED;
	fprintf(stderr, "usb-storage: driver stopped after an error\n");
	return 1;
}
