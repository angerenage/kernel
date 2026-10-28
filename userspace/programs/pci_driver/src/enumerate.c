#include "enumerate.h"

#include <runtime/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <system/capability.h>
#include <system/trace.h>

#include "claim.h"
#include "resource.h"

#define PCI_DEVICE_COUNT 32u
#define PCI_FUNCTION_COUNT 8u
#define PCI_BUS_COUNT 256u
#define PCI_INVALID_VENDOR_ID 0xffffu

#define PCI_VENDOR_ID_OFFSET 0x00u
#define PCI_DEVICE_ID_OFFSET 0x02u
#define PCI_REVISION_ID_OFFSET 0x08u
#define PCI_PROGRAMMING_INTERFACE_OFFSET 0x09u
#define PCI_SUBCLASS_OFFSET 0x0au
#define PCI_CLASS_CODE_OFFSET 0x0bu
#define PCI_HEADER_TYPE_OFFSET 0x0eu
#define PCI_SUBSYSTEM_VENDOR_ID_OFFSET 0x2cu
#define PCI_SUBSYSTEM_ID_OFFSET 0x2eu

#define PCI_HEADER_LAYOUT_MASK 0x7fu
#define PCI_HEADER_MULTIFUNCTION (1u << 7u)
#define PCI_HEADER_ENDPOINT 0u
#define PCI_HEADER_BRIDGE 1u
#define PCI_HEADER_CARDBUS 2u

#define PCI_CLASS_BRIDGE 0x06u
#define PCI_SUBCLASS_PCI_TO_PCI 0x04u

TRACE_LABEL(trace_claim_initialize, "pci/enumerate/claim_initialize");
TRACE_LABEL(trace_scan_bus, "pci/enumerate/scan_bus");
TRACE_LABEL(trace_function_read, "pci/enumerate/function_read");
TRACE_LABEL(trace_resource_detect, "pci/enumerate/resource_detect");
TRACE_LABEL(trace_function_print, "pci/enumerate/function_print");
TRACE_LABEL(trace_resource_claim, "pci/enumerate/resource_claim");

static bool function_read(const struct pci_config* config, struct pci_address address, uint16_t vendor_id,
                          struct pci_function* out_function) {
	struct pci_function function = {.address = address, .vendor_id = vendor_id};

	if (!pci_config_read16(config, address, PCI_DEVICE_ID_OFFSET, &function.device_id) ||
	    !pci_config_read8(config, address, PCI_REVISION_ID_OFFSET, &function.revision_id) ||
	    !pci_config_read8(config, address, PCI_PROGRAMMING_INTERFACE_OFFSET, &function.programming_interface) ||
	    !pci_config_read8(config, address, PCI_SUBCLASS_OFFSET, &function.subclass) ||
	    !pci_config_read8(config, address, PCI_CLASS_CODE_OFFSET, &function.class_code) ||
	    !pci_config_read8(config, address, PCI_HEADER_TYPE_OFFSET, &function.header_type))
		return false;
	if ((function.header_type & PCI_HEADER_LAYOUT_MASK) == PCI_HEADER_ENDPOINT) {
		if (!pci_config_read16(config, address, PCI_SUBSYSTEM_VENDOR_ID_OFFSET, &function.subsystem_vendor_id) ||
		    !pci_config_read16(config, address, PCI_SUBSYSTEM_ID_OFFSET, &function.subsystem_id))
			return false;
		function.has_subsystem_ids =
			function.subsystem_vendor_id != 0u && function.subsystem_vendor_id != PCI_INVALID_VENDOR_ID;
	}
	*out_function = function;
	return true;
}

static const char* function_kind(const struct pci_function* function) {
	uint8_t layout = function->header_type & PCI_HEADER_LAYOUT_MASK;
	if (function->class_code == PCI_CLASS_BRIDGE && function->subclass == PCI_SUBCLASS_PCI_TO_PCI)
		return "PCI-to-PCI bridge";
	if (layout == PCI_HEADER_ENDPOINT) return "endpoint";
	if (layout == PCI_HEADER_BRIDGE) return "bridge header";
	if (layout == PCI_HEADER_CARDBUS) return "CardBus bridge";
	return "unknown header";
}

static void function_print(const struct pci_function* function) {
	RUNTIME_DEBUG_LOG("pci: %04x:%02x:%02x.%u %04x:%04x class=%02x:%02x:%02x revision=%02x (%s)\n",
	                  function->address.segment,
	                  function->address.bus,
	                  function->address.device,
	                  function->address.function,
	                  function->vendor_id,
	                  function->device_id,
	                  function->class_code,
	                  function->subclass,
	                  function->programming_interface,
	                  function->revision_id,
	                  function_kind(function));
}

bool pci_enumerate(struct pci_host* host, cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                   struct pci_inventory* out_inventory) {
	struct pci_claim_context claims;
	struct pci_inventory     inventory = {0};
	uint8_t                  bus_queue[PCI_BUS_COUNT];
	bool                     bus_queued[PCI_BUS_COUNT] = {false};
	size_t                   bus_head                  = 0u;
	size_t                   bus_tail                  = 0u;
	bool                     success;

	if (host == NULL || out_inventory == NULL) return false;
	{
		TRACE_SCOPE(trace, &trace_claim_initialize);
		success = pci_claim_initialize(&host->ranges, memory_allocator_cap, interrupts_cap, io_ports_cap, &claims);
	}
	if (!success) return false;
	bus_queue[bus_tail++]       = host->first_bus;
	bus_queued[host->first_bus] = true;
	while (bus_head < bus_tail) {
		uint8_t bus = bus_queue[bus_head++];
		TRACE_SCOPE(bus_trace, &trace_scan_bus);
		for (uint8_t device = 0u; device < PCI_DEVICE_COUNT; device++) {
			struct pci_address function_zero = {.segment = host->segment, .bus = bus, .device = device, .function = 0u};
			uint16_t           vendor;
			uint8_t            header_type;
			uint8_t            function_count;

			if (!pci_config_read16(&host->config, function_zero, PCI_VENDOR_ID_OFFSET, &vendor)) goto fail;
			if (vendor == 0u || vendor == PCI_INVALID_VENDOR_ID) continue;
			if (!pci_config_read8(&host->config, function_zero, PCI_HEADER_TYPE_OFFSET, &header_type)) goto fail;
			function_count = (header_type & PCI_HEADER_MULTIFUNCTION) != 0u ? PCI_FUNCTION_COUNT : 1u;
			for (uint8_t function_index = 0u; function_index < function_count; function_index++) {
				struct pci_address   address = function_zero;
				struct pci_function* function;

				address.function = function_index;
				if (!pci_config_read16(&host->config, address, PCI_VENDOR_ID_OFFSET, &vendor)) goto fail;
				if (vendor == 0u || vendor == PCI_INVALID_VENDOR_ID) continue;
				function = calloc(1u, sizeof(*function));
				if (function == NULL) goto fail;
				{
					TRACE_SCOPE(trace, &trace_function_read);
					success = function_read(&host->config, address, vendor, function);
				}
				if (!success) {
					free(function);
					goto fail;
				}
				{
					TRACE_SCOPE(trace, &trace_resource_detect);
					success = pci_resources_detect(&host->config, address, function->header_type, &function->resources);
				}
				if (!success) {
					free(function);
					goto fail;
				}
				if (function->resources.bridge.present) {
					uint8_t secondary   = function->resources.bridge.secondary_bus;
					uint8_t subordinate = function->resources.bridge.subordinate_bus;

					if (secondary >= host->first_bus && secondary <= host->last_bus && secondary <= subordinate &&
					    !bus_queued[secondary]) {
						bus_queue[bus_tail++] = secondary;
						bus_queued[secondary] = true;
					}
				}
				{
					TRACE_SCOPE(trace, &trace_function_print);
					function_print(function);
				}
				{
					TRACE_SCOPE(trace, &trace_resource_claim);
					inventory.resource_count += pci_resources_claim(&claims, &address, &function->resources);
				}
				if (inventory.functions_tail == NULL) inventory.functions = function;
				else inventory.functions_tail->next = function;
				inventory.functions_tail = function;
				inventory.function_count++;
			}
		}
	}
	pci_claim_deinitialize(&claims);
	*out_inventory = inventory;
	return true;

fail:
	pci_claim_deinitialize(&claims);
	pci_inventory_deinitialize(&inventory);
	return false;
}

void pci_inventory_deinitialize(struct pci_inventory* inventory) {
	if (inventory == NULL) return;
	while (inventory->functions != NULL) {
		struct pci_function* next = inventory->functions->next;
		pci_resources_release(&inventory->functions->resources);
		free(inventory->functions);
		inventory->functions = next;
	}
	*inventory = (struct pci_inventory){0};
}
