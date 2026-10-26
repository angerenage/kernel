#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Probe one partition, print its FAT32 root directory, and remain bound. */
enum device_driver_result fat32_driver_run(cap_id_t device_cap);
