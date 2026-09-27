#pragma once

#include <base/hardware/pci.h>
#include <stdbool.h>
#include <stddef.h>

/* Return the number of PCI controllers discovered from all supported sources. */
size_t kernel_hardware_pci_count(void);

/* Return one PCI controller by stable discovery index. */
bool kernel_hardware_pci_get(size_t index, struct pci_controller* out_controller);
