#include <base/cap.h>
#include <protocol/device_manager.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#include "bootstrap.h"
#include "server.h"
#include "source.h"

static bool release_source(cap_id_t capability, const char* name) {
	if (capability == CAP_ID_INVALID) return true;
	if (cap_drop(capability) == SYSCALL_STATUS_OK) return true;
	printf("device-manager: failed to release %s source\n", name);
	return false;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct device_manager_source_selection selection;
	enum device_manager_source_result      selection_result;
	struct device_server                   server;
	cap_id_t                               acpi_cap;
	cap_id_t                               device_tree_cap;
	cap_id_t                               io_ports_cap;
	cap_id_t                               unused_cap;

	if (capc != DEVICE_MANAGER_CAPABILITY_COUNT || capv == NULL) return 1;
	acpi_cap         = capv[DEVICE_MANAGER_CAPABILITY_ACPI];
	device_tree_cap  = capv[DEVICE_MANAGER_CAPABILITY_DEVICE_TREE];
	io_ports_cap     = capv[DEVICE_MANAGER_CAPABILITY_IO_PORTS];
	selection_result = device_manager_source_select(argc, argv, acpi_cap, device_tree_cap, &selection);
	if (selection_result != DEVICE_MANAGER_SOURCE_OK) {
		printf("device-manager: firmware source selection failed: %u\n", (unsigned)selection_result);
		(void)release_source(acpi_cap, "ACPI");
		if (device_tree_cap != acpi_cap) (void)release_source(device_tree_cap, "Device Tree");
		(void)release_source(io_ports_cap, "I/O ports");
		return 1;
	}
	unused_cap = selection.source == DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE ? acpi_cap : device_tree_cap;
	const char* unused_name = selection.source == DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE ? "ACPI" : "Device Tree";
	if (unused_cap != CAP_ID_INVALID && unused_cap != selection.capability &&
	    !release_source(unused_cap, unused_name)) {
		(void)release_source(selection.capability, "selected firmware");
		(void)release_source(io_ports_cap, "I/O ports");
		return 1;
	}
	if (!device_server_init(&server)) {
		printf("device-manager: initialization failed\n");
		(void)release_source(selection.capability, "selected firmware");
		(void)release_source(io_ports_cap, "I/O ports");
		return 1;
	}
	printf("device-manager: selected %s firmware\n",
	       selection.source == DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE ? "Device Tree" : "ACPI");
	if (!device_manager_parser_launch(&server, selection.source, &selection.capability, io_ports_cap)) {
		(void)release_source(selection.capability, "selected firmware");
		(void)release_source(io_ports_cap, "I/O ports");
		device_server_deinit(&server);
		return 1;
	}
	if (!release_source(io_ports_cap, "I/O ports")) {
		device_server_deinit(&server);
		return 1;
	}
	int result = device_server_run(&server);
	device_server_deinit(&server);
	return result;
}
