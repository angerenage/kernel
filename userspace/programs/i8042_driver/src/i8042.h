#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Acquire, initialize, and serve one ACPI-described i8042 controller. */
enum device_driver_result i8042_driver_run(cap_id_t device_cap);
