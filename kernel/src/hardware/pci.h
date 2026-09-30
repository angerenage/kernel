#pragma once

#include <stdbool.h>

/* Reconcile firmware PCI controllers and register their standard device descriptors. */
bool kernel_device_register_pci(void);
