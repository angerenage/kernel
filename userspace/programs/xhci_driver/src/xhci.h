#pragma once

#include <base/cap.h>

#include <protocol/device.h>

/* Inspect and retain one PCI xHCI controller. */
enum device_driver_result xhci_driver_run(cap_id_t device_cap);
