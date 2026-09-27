#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>

/* Create the PCI record capability object when controllers are available. */
bool kernel_capability_pci_init(void);

/* Report whether PCI controller records are available. */
bool kernel_capability_pci_available(void);

/* Grant read access to the PCI controller records. */
cap_id_t kernel_capability_pci_grant(process_id_t recipient);
