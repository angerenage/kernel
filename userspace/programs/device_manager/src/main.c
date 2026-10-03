#include <base/cap.h>
#include <protocol/device_manager.h>
#include <stddef.h>
#include <stdio.h>

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	if (capc != DEVICE_MANAGER_CAPABILITY_COUNT || capv == NULL) return 1;
	cap_id_t acpi_cap        = capv[DEVICE_MANAGER_CAPABILITY_ACPI];
	cap_id_t device_tree_cap = capv[DEVICE_MANAGER_CAPABILITY_DEVICE_TREE];
	(void)acpi_cap;
	(void)device_tree_cap;
	printf("device-manager: started\n");
	return 0;
}
