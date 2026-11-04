#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stdbool.h>

/* Ask the PCI owner to enable DMA and the function's implemented BAR decoders. */
syscall_status_t pci_control_set_dma_enabled(cap_id_t control_cap, bool enabled, uint16_t* out_command);
