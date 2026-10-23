#pragma once

#include <stdbool.h>

#include "host.h"

/* Scan the firmware-described bus range and print each discovered PCI function. */
bool pci_enumerate(const struct pci_host* host);
