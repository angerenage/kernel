#pragma once

/* Positional capabilities accepted by the device manager at startup. */
enum device_manager_capability_argument {
	DEVICE_MANAGER_CAPABILITY_ACPI = 0u,
	DEVICE_MANAGER_CAPABILITY_DEVICE_TREE,
	DEVICE_MANAGER_CAPABILITY_COUNT,
};
