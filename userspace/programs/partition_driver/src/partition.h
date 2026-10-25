#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Open a block:disk and publish each discovered partition as a child device. */
bool partition_driver_run(cap_id_t device_cap);
