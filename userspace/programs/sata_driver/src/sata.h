#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Identify and serve one ATA disk exposed by an AHCI port. */
enum device_driver_result sata_driver_run(cap_id_t device_cap);
