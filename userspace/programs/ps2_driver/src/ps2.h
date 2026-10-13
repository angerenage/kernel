#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stdint.h>

/* Device protocol selected from reset and Identify responses. */
enum ps2_device_kind {
	PS2_DEVICE_UNKNOWN = 0u,
	PS2_DEVICE_KEYBOARD,
	PS2_DEVICE_MOUSE,
};

/* Acquire, identify, configure, and serve one controller-independent PS/2 child. */
bool ps2_driver_run(cap_id_t device_cap);
