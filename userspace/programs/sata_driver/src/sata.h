#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Identify and serve one ATA disk exposed by an AHCI port. */
bool sata_driver_run(cap_id_t device_cap);
