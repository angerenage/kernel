#pragma once

#include <base/cap.h>

#include <protocol/device.h>

/* Run a SATAPI device driver on a given device and publish a block-device view of the media. */
enum device_driver_result satapi_driver_run(cap_id_t device_cap);
