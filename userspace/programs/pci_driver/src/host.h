#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "ecam.h"
#include "ranges.h"

/* One validated and mapped firmware-described PCI host bridge. */
struct pci_host {
	uint16_t               segment;
	uint8_t                first_bus;
	uint8_t                last_bus;
	cap_id_t               memory_cap;
	cap_id_t               address_space_cap;
	size_t                 configuration_size;
	struct pci_host_ranges ranges;
	struct pci_ecam        ecam;
	struct pci_config      config;
};

/* Acquire, validate, map, and configure one firmware-described ECAM host. */
bool pci_host_initialize(cap_id_t device_cap, struct pci_host* out_host);

/* Unmap and release every capability owned by a PCI host. */
void pci_host_deinitialize(struct pci_host* host);
