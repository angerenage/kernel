#include "enumerate.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PCI_DEVICE_COUNT 32u
#define PCI_FUNCTION_COUNT 8u
#define PCI_INVALID_VENDOR_ID 0xffffu

#define PCI_VENDOR_ID_OFFSET 0x00u
#define PCI_DEVICE_ID_OFFSET 0x02u
#define PCI_REVISION_ID_OFFSET 0x08u
#define PCI_PROGRAMMING_INTERFACE_OFFSET 0x09u
#define PCI_SUBCLASS_OFFSET 0x0au
#define PCI_CLASS_CODE_OFFSET 0x0bu
#define PCI_HEADER_TYPE_OFFSET 0x0eu

#define PCI_HEADER_LAYOUT_MASK 0x7fu
#define PCI_HEADER_MULTIFUNCTION (1u << 7u)
#define PCI_HEADER_ENDPOINT 0u
#define PCI_HEADER_BRIDGE 1u
#define PCI_HEADER_CARDBUS 2u

#define PCI_CLASS_BRIDGE 0x06u
#define PCI_SUBCLASS_PCI_TO_PCI 0x04u

struct pci_function {
	struct pci_address address;
	uint16_t           vendor_id;
	uint16_t           device_id;
	uint8_t            revision_id;
	uint8_t            programming_interface;
	uint8_t            subclass;
	uint8_t            class_code;
	uint8_t            header_type;
};

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
	printf("pci: %04x:%02x:%02x.%u %04x:%04x class=%02x:%02x:%02x revision=%02x (%s)\n",
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

bool pci_enumerate(const struct pci_host* host) {
	if (host == NULL) return false;
	for (uint16_t bus = host->first_bus; bus <= host->last_bus; bus++) {
		for (uint8_t device = 0u; device < PCI_DEVICE_COUNT; device++) {
			struct pci_address function_zero = {
				.segment = host->segment, .bus = (uint8_t)bus, .device = device, .function = 0u};
			uint16_t vendor;
			uint8_t  header_type;
			uint8_t  function_count;

			if (!pci_config_read16(&host->config, function_zero, PCI_VENDOR_ID_OFFSET, &vendor)) return false;
			if (vendor == 0u || vendor == PCI_INVALID_VENDOR_ID) continue;
			if (!pci_config_read8(&host->config, function_zero, PCI_HEADER_TYPE_OFFSET, &header_type)) return false;
			function_count = (header_type & PCI_HEADER_MULTIFUNCTION) != 0u ? PCI_FUNCTION_COUNT : 1u;
			for (uint8_t function_index = 0u; function_index < function_count; function_index++) {
				struct pci_address  address = function_zero;
				struct pci_function function;

				address.function = function_index;
				if (!pci_config_read16(&host->config, address, PCI_VENDOR_ID_OFFSET, &vendor)) return false;
				if (vendor == 0u || vendor == PCI_INVALID_VENDOR_ID) continue;
				if (!function_read(&host->config, address, vendor, &function)) return false;
				function_print(&function);
			}
		}
		if (bus == UINT8_MAX) break;
	}
	return true;
}
