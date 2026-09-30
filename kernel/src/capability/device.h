#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Freeze the boot-time inventory and create the singleton Devices resource. */
bool kernel_capability_devices_init(void);

/* Grant read access to the typed device inventory. */
cap_id_t kernel_capability_devices_grant(process_id_t recipient);
