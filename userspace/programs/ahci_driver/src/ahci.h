#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Validate and retain one PCI AHCI controller while preparing its DMA context. */
bool ahci_driver_run(cap_id_t device_cap);
