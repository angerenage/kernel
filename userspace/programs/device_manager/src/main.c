#include <base/cap.h>
#include <protocol/device_manager.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#include "server.h"

static bool release_source(cap_id_t capability, const char* name) {
	if (capability == CAP_ID_INVALID) return true;
	if (cap_drop(capability) == SYSCALL_STATUS_OK) return true;
	printf("device-manager: failed to release unused %s source\n", name);
	return false;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct device_server server;
	(void)argc;
	(void)argv;
	if (capc != DEVICE_MANAGER_CAPABILITY_COUNT || capv == NULL) return 1;
	cap_id_t acpi_cap         = capv[DEVICE_MANAGER_CAPABILITY_ACPI];
	cap_id_t device_tree_cap  = capv[DEVICE_MANAGER_CAPABILITY_DEVICE_TREE];
	bool     sources_released = release_source(acpi_cap, "ACPI");
	if (!release_source(device_tree_cap, "Device Tree")) sources_released = false;
	if (!sources_released) return 1;
	if (!device_server_init(&server)) {
		printf("device-manager: initialization failed\n");
		return 1;
	}
	printf("device-manager: started\n");
	int result = device_server_run(&server);
	device_server_deinit(&server);
	return result;
}
