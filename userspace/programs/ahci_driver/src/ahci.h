#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Validate, inspect, and retain one PCI AHCI controller without changing its state. */
bool ahci_driver_run(cap_id_t device_cap);
