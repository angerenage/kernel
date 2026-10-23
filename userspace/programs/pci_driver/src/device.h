#pragma once

#include <base/cap.h>
#include <base/syscall.h>

#include "enumerate.h"

/* Publish every retained function below its PCI host device. */
syscall_status_t pci_devices_publish(cap_id_t host_cap, struct pci_inventory* inventory);
