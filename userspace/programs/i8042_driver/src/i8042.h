#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Acquire, initialize, and serve one ACPI-described i8042 controller. */
bool i8042_driver_run(cap_id_t device_cap);
