#pragma once

#include <stdbool.h>

#include "host.h"
#include "resource.h"

/* One discovered function retained until it can be published to the device manager. */
struct pci_function {
	struct pci_address   address;
	uint16_t             vendor_id;
	uint16_t             device_id;
	uint16_t             subsystem_vendor_id;
	uint16_t             subsystem_id;
	uint8_t              revision_id;
	uint8_t              programming_interface;
	uint8_t              subclass;
	uint8_t              class_code;
	uint8_t              header_type;
	bool                 has_subsystem_ids;
	struct pci_resources resources;
	struct pci_function* next;
};

/* Claimed resources and functions discovered below one host bridge. */
struct pci_inventory {
	struct pci_function* functions;
	struct pci_function* functions_tail;
	size_t               function_count;
	size_t               resource_count;
};

/* Scan the firmware-described bus range and retain every discovered function. */
bool pci_enumerate(struct pci_host* host, cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                   struct pci_inventory* out_inventory);

/* Drop claimed resources and release an inventory. */
void pci_inventory_deinitialize(struct pci_inventory* inventory);
