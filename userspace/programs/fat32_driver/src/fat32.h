#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Probe one partition and serve it through the filesystem protocol. */
enum device_driver_result fat32_driver_run(cap_id_t device_cap);
