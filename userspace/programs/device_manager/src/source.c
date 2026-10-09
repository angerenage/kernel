#include "source.h"

#include <base/device_tree.h>
#include <protocol/device_manager.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <system/device_tree.h>

enum source_preference {
	SOURCE_PREFERENCE_AUTO = 0u,
	SOURCE_PREFERENCE_DEVICE_TREE,
	SOURCE_PREFERENCE_ACPI,
};

static enum device_manager_source_result parse_preference(int argc, char** argv,
                                                          enum source_preference* out_preference) {
	static const char option[] = DEVICE_MANAGER_SOURCE_OPTION;
	static const char prefix[] = DEVICE_MANAGER_SOURCE_OPTION "=";
	const char*       value    = NULL;

	if (out_preference == NULL || argc < 0 || (argc != 0 && argv == NULL))
		return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
	*out_preference = SOURCE_PREFERENCE_AUTO;
	for (int index = 0; index < argc; index++) {
		size_t argument_size;

		if (argv[index] == NULL) return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
		if (strcmp(argv[index], option) == 0) return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
		argument_size = strlen(argv[index]);
		if (argument_size < sizeof(prefix) - 1u || memcmp(argv[index], prefix, sizeof(prefix) - 1u) != 0) continue;
		if (value != NULL) return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
		value = argv[index] + sizeof(prefix) - 1u;
	}
	if (value == NULL) return DEVICE_MANAGER_SOURCE_OK;
	if (strcmp(value, DEVICE_MANAGER_SOURCE_AUTO) == 0) return DEVICE_MANAGER_SOURCE_OK;
	if (strcmp(value, DEVICE_MANAGER_SOURCE_DEVICE_TREE) == 0) {
		*out_preference = SOURCE_PREFERENCE_DEVICE_TREE;
		return DEVICE_MANAGER_SOURCE_OK;
	}
	if (strcmp(value, DEVICE_MANAGER_SOURCE_ACPI) == 0) {
		*out_preference = SOURCE_PREFERENCE_ACPI;
		return DEVICE_MANAGER_SOURCE_OK;
	}
	return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
}

static bool device_tree_valid(cap_id_t capability) {
	device_tree_node_id_t root = DEVICE_TREE_NODE_INVALID;

	return capability != CAP_ID_INVALID && device_tree_root(capability, &root) == SYSCALL_STATUS_OK &&
	       root != DEVICE_TREE_NODE_INVALID && root != 0u;
}

enum device_manager_source_result device_manager_source_select(int argc, char** argv, cap_id_t acpi_cap,
                                                               cap_id_t                                device_tree_cap,
                                                               struct device_manager_source_selection* out_selection) {
	enum source_preference            preference;
	enum device_manager_source_result result;
	bool                              dt_valid;

	if (out_selection == NULL) return DEVICE_MANAGER_SOURCE_INVALID_ARGUMENT;
	*out_selection = (struct device_manager_source_selection){
		.source     = DEVICE_MANAGER_FIRMWARE_SOURCE_INVALID,
		.capability = CAP_ID_INVALID,
	};
	result = parse_preference(argc, argv, &preference);
	if (result != DEVICE_MANAGER_SOURCE_OK) return result;
	if (preference == SOURCE_PREFERENCE_ACPI) {
		if (acpi_cap == CAP_ID_INVALID) return DEVICE_MANAGER_SOURCE_UNAVAILABLE;
		*out_selection = (struct device_manager_source_selection){
			.source     = DEVICE_MANAGER_FIRMWARE_SOURCE_ACPI,
			.capability = acpi_cap,
		};
		return DEVICE_MANAGER_SOURCE_OK;
	}
	dt_valid = device_tree_valid(device_tree_cap);
	if (preference == SOURCE_PREFERENCE_DEVICE_TREE) {
		if (!dt_valid) return DEVICE_MANAGER_SOURCE_UNAVAILABLE;
		*out_selection = (struct device_manager_source_selection){
			.source     = DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE,
			.capability = device_tree_cap,
		};
		return DEVICE_MANAGER_SOURCE_OK;
	}
	if (dt_valid) {
		*out_selection = (struct device_manager_source_selection){
			.source     = DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE,
			.capability = device_tree_cap,
		};
		return DEVICE_MANAGER_SOURCE_OK;
	}
	if (acpi_cap != CAP_ID_INVALID) {
		*out_selection = (struct device_manager_source_selection){
			.source     = DEVICE_MANAGER_FIRMWARE_SOURCE_ACPI,
			.capability = acpi_cap,
		};
		return DEVICE_MANAGER_SOURCE_OK;
	}
	return DEVICE_MANAGER_SOURCE_UNAVAILABLE;
}
