#pragma once

#include "init.h"

enum device_manager_launch_result {
	DEVICE_MANAGER_LAUNCH_SUCCESS = 0,
	DEVICE_MANAGER_LAUNCH_UNAVAILABLE,
	DEVICE_MANAGER_LAUNCH_FAILED,
};

/* Load and start the device manager through the userspace loader. */
enum device_manager_launch_result device_manager_launch(const struct init_state* init);
