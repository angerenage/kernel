#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Open a block:disk and publish each discovered partition as a child device. */
enum device_driver_result partition_driver_run(cap_id_t device_cap);
