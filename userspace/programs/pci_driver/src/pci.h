#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Enumerate one firmware-described PCI ECAM host and publish its functions. */
bool pci_driver_run(cap_id_t device_cap, cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap);
