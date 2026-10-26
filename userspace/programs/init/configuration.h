#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>

#define INIT_CONFIGURATION_LAYOUT_PATH_CAPACITY 256u
#define INIT_CONFIGURATION_MAX_MOUNTS 16u
#define INIT_CONFIGURATION_SERVICE_CAPACITY 64u
#define INIT_CONFIGURATION_MOUNT_PATH_CAPACITY 256u

struct init_mount_configuration {
	char service[INIT_CONFIGURATION_SERVICE_CAPACITY];
	char mount_point[INIT_CONFIGURATION_MOUNT_PATH_CAPACITY];
	bool read_only;
};

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
	char                            text_input_layout[INIT_CONFIGURATION_LAYOUT_PATH_CAPACITY];
	size_t                          mount_count;
	struct init_mount_configuration mounts[INIT_CONFIGURATION_MAX_MOUNTS];
};

/* Load the version-one YAML configuration from an absolute VFS path. */
enum init_configuration_status init_configuration_load(cap_id_t vfs_cap, const char* path,
                                                       struct init_configuration* configuration);

/* Return a stable diagnostic string for one configuration result. */
const char* init_configuration_status_string(enum init_configuration_status status);
