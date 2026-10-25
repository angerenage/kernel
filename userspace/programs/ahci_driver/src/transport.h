#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ahci_dma_context;
struct ahci_host_registers;

/* Publish populated AHCI ports as child devices and serve their transports. */
bool ahci_transport_run(cap_id_t device_cap, volatile struct ahci_host_registers* registers, size_t bar_size,
                        uint32_t capabilities, uint32_t implemented, const struct ahci_dma_context* dma);
