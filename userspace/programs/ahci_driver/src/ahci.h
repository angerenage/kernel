#pragma once

#include <base/cap.h>
#include <protocol/device.h>

/* Validate and retain one PCI AHCI controller while preparing its DMA context. */
enum device_driver_result ahci_driver_run(cap_id_t device_cap);
