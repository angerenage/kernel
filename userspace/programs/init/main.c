#include <base/startup.h>
#include <stdio.h>

#include "server.h"

extern struct init_startup_info g_startup;

int main() {
	if (g_startup.loader_cap == CAP_ID_INVALID) {
		printf("init: loader capability not available\n");
		return 1;
	}
	if (g_startup.kernel_resources_cap == CAP_ID_INVALID) {
		printf("init: kernel-resources capability not available\n");
		return 1;
	}
	if (g_startup.base.serial_cap == CAP_ID_INVALID) {
		printf("init: serial capability not available\n");
		return 1;
	}

	if (!server_init()) return 1;
	int result = server_run(&g_startup);
	server_deinit();
	return result;
}
