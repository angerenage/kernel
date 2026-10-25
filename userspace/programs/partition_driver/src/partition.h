#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Open a block:disk child and perform the initial read-only partition probe. */
bool partition_driver_run(cap_id_t device_cap);
