#pragma once

#include <base/cap.h>

#define INIT_CONFIGURATION_LAYOUT_PATH_CAPACITY 256u

/* Result of loading and validating the system configuration. */
enum init_configuration_status {
	INIT_CONFIGURATION_OK,
	INIT_CONFIGURATION_IO_ERROR,
	INIT_CONFIGURATION_YAML_ERROR,
	INIT_CONFIGURATION_INVALID,
	INIT_CONFIGURATION_NO_MEMORY,
};

/* System-wide settings selected before launching bootstrap programs. */
struct init_configuration {
	char text_input_layout[INIT_CONFIGURATION_LAYOUT_PATH_CAPACITY];
};

/* Load the version-one YAML configuration from an absolute VFS path. */
enum init_configuration_status init_configuration_load(cap_id_t vfs_cap, const char* path,
                                                       struct init_configuration* configuration);

/* Return a stable diagnostic string for one configuration result. */
const char* init_configuration_status_string(enum init_configuration_status status);
