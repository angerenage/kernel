#pragma once

#include <base/cap.h>

/* Firmware source selected for this boot's single parser process. */
enum device_manager_firmware_source {
	DEVICE_MANAGER_FIRMWARE_SOURCE_INVALID = 0u,
	DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE,
	DEVICE_MANAGER_FIRMWARE_SOURCE_ACPI,
};

/* Domain result returned while parsing the override and selecting an available source. */
enum device_manager_source_result {
	DEVICE_MANAGER_SOURCE_OK = 0u,
	DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT,
	DEVICE_MANAGER_SOURCE_UNAVAILABLE,
};

/* Selected source kind and manager-owned firmware capability. */
struct device_manager_source_selection {
	enum device_manager_firmware_source source;
	cap_id_t                            capability;
};

/* Select a valid firmware source, honoring at most one command-line override. */
enum device_manager_source_result device_manager_source_select(int argc, char** argv, cap_id_t acpi_cap,
                                                               cap_id_t                                device_tree_cap,
                                                               struct device_manager_source_selection* out_selection);
