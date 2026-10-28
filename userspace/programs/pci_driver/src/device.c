#include "device.h"

#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/trace.h>

#include "claim.h"
#include "resource.h"

#define PCI_HEADER_LAYOUT_MASK 0x7fu
#define PCI_HEADER_MULTIFUNCTION (1u << 7u)
#define PCI_COMMAND_IO_ENABLE (1u << 0u)
#define PCI_COMMAND_MEMORY_ENABLE (1u << 1u)

#define PCI_MEMORY_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define PCI_IO_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define PCI_INTERRUPT_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE))

TRACE_LABEL(trace_publish_device, "pci/publish/device");
TRACE_LABEL(trace_publish_begin, "pci/publish/begin");
TRACE_LABEL(trace_publish_properties, "pci/publish/properties");
TRACE_LABEL(trace_publish_resources, "pci/publish/resources");
TRACE_LABEL(trace_publish_set_name, "pci/publish/set_name");
TRACE_LABEL(trace_publish_compatible, "pci/publish/compatible");
TRACE_LABEL(trace_publish_identity, "pci/publish/identity");
TRACE_LABEL(trace_publish_bars, "pci/publish/bars");
TRACE_LABEL(trace_publish_rom, "pci/publish/rom");
TRACE_LABEL(trace_publish_interrupts, "pci/publish/interrupts");
TRACE_LABEL(trace_publish_msix, "pci/publish/msix");
TRACE_LABEL(trace_publish_bridge, "pci/publish/bridge");
TRACE_LABEL(trace_publish_dma, "pci/publish/dma");
TRACE_LABEL(trace_publish_commit, "pci/publish/commit");

static syscall_status_t add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t encoded[sizeof(value)];

	for (size_t index = 0u; index < sizeof(encoded); index++) encoded[index] = (uint8_t)(value >> (index * 8u));
	return device_builder_add_property(
		builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, encoded, sizeof(encoded));
}

static syscall_status_t add_bool(const struct device_builder* builder, const char* name, bool value) {
	uint8_t encoded = value ? 1u : 0u;
	return device_builder_add_property(
		builder, name, strlen(name), DEVICE_PROPERTY_BOOLEAN, 1u, &encoded, sizeof(encoded));
}

static bool format(char* buffer, size_t capacity, size_t* out_size, const char* pattern, ...) {
	va_list arguments;
	va_start(arguments, pattern);
	int written = vsnprintf(buffer, capacity, pattern, arguments);
	va_end(arguments);
	if (written <= 0 || (size_t)written >= capacity) return false;
	*out_size = (size_t)written;
	return true;
}

static syscall_status_t add_compatible_ids(const struct device_builder* builder, const struct pci_function* function) {
	char             compatible[48];
	size_t           size;
	syscall_status_t status;

	if (function->has_subsystem_ids) {
		if (!format(compatible,
		            sizeof(compatible),
		            &size,
		            "pci:%04x:%04x:%04x:%04x",
		            function->vendor_id,
		            function->device_id,
		            function->subsystem_vendor_id,
		            function->subsystem_id))
			return SYSCALL_STATUS_FAILED;
		status = device_builder_add_compatible(builder, compatible, size);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	if (!format(compatible, sizeof(compatible), &size, "pci:%04x:%04x", function->vendor_id, function->device_id))
		return SYSCALL_STATUS_FAILED;
	status = device_builder_add_compatible(builder, compatible, size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (!format(compatible,
	            sizeof(compatible),
	            &size,
	            "pci:class:%02x:%02x:%02x",
	            function->class_code,
	            function->subclass,
	            function->programming_interface))
		return SYSCALL_STATUS_FAILED;
	status = device_builder_add_compatible(builder, compatible, size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (!format(compatible, sizeof(compatible), &size, "pci:class:%02x:%02x", function->class_code, function->subclass))
		return SYSCALL_STATUS_FAILED;
	status = device_builder_add_compatible(builder, compatible, size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (!format(compatible, sizeof(compatible), &size, "pci:class:%02x", function->class_code))
		return SYSCALL_STATUS_FAILED;
	status = device_builder_add_compatible(builder, compatible, size);
	if (status != SYSCALL_STATUS_OK) return status;
	return device_builder_add_compatible(builder, "pci:device", sizeof("pci:device") - 1u);
}

static syscall_status_t add_identity(const struct device_builder* builder, const struct pci_function* function) {
	syscall_status_t status = add_u64(builder, "segment_group", function->address.segment);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "bus", function->address.bus);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "device", function->address.device);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "function", function->address.function);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "requester_id", function->resources.requester_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "vendor_id", function->vendor_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "device_id", function->device_id);
	if (status == SYSCALL_STATUS_OK && function->has_subsystem_ids)
		status = add_u64(builder, "subsystem_vendor_id", function->subsystem_vendor_id);
	if (status == SYSCALL_STATUS_OK && function->has_subsystem_ids)
		status = add_u64(builder, "subsystem_id", function->subsystem_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "revision_id", function->revision_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "class_code", function->class_code);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "subclass", function->subclass);
	if (status == SYSCALL_STATUS_OK)
		status = add_u64(builder, "programming_interface", function->programming_interface);
	if (status == SYSCALL_STATUS_OK)
		status = add_u64(builder, "header_type", function->header_type & PCI_HEADER_LAYOUT_MASK);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(builder, "multifunction", (function->header_type & PCI_HEADER_MULTIFUNCTION) != 0u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "command", function->resources.command);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "status", function->resources.status);
	return status;
}

static syscall_status_t add_bar(const struct device_builder* builder, const struct pci_resources* resources,
                                const struct pci_bar_resource* bar) {
	char             name[32];
	size_t           size;
	syscall_status_t status;
	bool             memory = bar->kind == PCI_BAR_MEMORY32 || bar->kind == PCI_BAR_MEMORY64;

	if (!format(name, sizeof(name), &size, "bar_%u_kind", bar->index)) return SYSCALL_STATUS_FAILED;
	status = add_u64(builder, name, bar->kind);
	if (!format(name, sizeof(name), &size, "bar_%u_address", bar->index)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, name, bar->address);
	if (!format(name, sizeof(name), &size, "bar_%u_size", bar->index)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, name, bar->size);
	if (!format(name, sizeof(name), &size, "bar_%u_enabled", bar->index)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(
			builder,
			name,
			(resources->command & (bar->kind == PCI_BAR_IO ? PCI_COMMAND_IO_ENABLE : PCI_COMMAND_MEMORY_ENABLE)) != 0u);
	if (!format(name, sizeof(name), &size, "bar_%u_claimed", bar->index)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, name, bar->capability != CAP_ID_INVALID);
	if (!format(name, sizeof(name), &size, "bar_%u_mapping_offset", bar->index)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, name, bar->mapping_offset);
	if (memory) {
		if (!format(name, sizeof(name), &size, "bar_%u_prefetchable", bar->index)) return SYSCALL_STATUS_FAILED;
		if (status == SYSCALL_STATUS_OK) status = add_bool(builder, name, bar->prefetchable);
	}
	if (status != SYSCALL_STATUS_OK || bar->capability == CAP_ID_INVALID) return status;
	if (!format(name, sizeof(name), &size, "bar_%u", bar->index)) return SYSCALL_STATUS_FAILED;
	return device_builder_add_resource(builder,
	                                   name,
	                                   size,
	                                   bar->capability,
	                                   bar->kind == PCI_BAR_IO ? PCI_IO_RESOURCE_RIGHTS : PCI_MEMORY_RESOURCE_RIGHTS);
}

static syscall_status_t add_bars(const struct device_builder* builder, const struct pci_resources* resources) {
	for (uint8_t index = 0u; index < resources->bar_count; index++) {
		syscall_status_t status = add_bar(builder, resources, &resources->bars[index]);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t add_rom(const struct device_builder* builder, const struct pci_rom_resource* rom) {
	if (!rom->present) return SYSCALL_STATUS_OK;
	syscall_status_t status = add_u64(builder, "expansion_rom_address", rom->address);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "expansion_rom_size", rom->size);
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, "expansion_rom_enabled", rom->enabled);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(builder, "expansion_rom_claimed", rom->capability != CAP_ID_INVALID);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "expansion_rom_mapping_offset", rom->mapping_offset);
	if (status == SYSCALL_STATUS_OK && rom->capability != CAP_ID_INVALID)
		status = device_builder_add_resource(
			builder, "expansion_rom", sizeof("expansion_rom") - 1u, rom->capability, PCI_MEMORY_RESOURCE_RIGHTS);
	return status;
}

static syscall_status_t add_interrupts(const struct device_builder* builder, const struct pci_resources* resources) {
	syscall_status_t status = SYSCALL_STATUS_OK;

	if (resources->intx.pin != 0u) {
		status = add_u64(builder, "interrupt_pin", resources->intx.pin);
		if (status == SYSCALL_STATUS_OK && resources->intx.line != UINT8_MAX)
			status = add_u64(builder, "interrupt_line", resources->intx.line);
	}
	if (status != SYSCALL_STATUS_OK || !resources->msi.present) return status;
	status = add_u64(builder, "msi_capability_offset", resources->msi.capability_offset);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msi_vector_count", resources->msi.vector_count);
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, "msi_enabled", resources->msi.enabled);
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, "msi_address_64bit", resources->msi.address_64bit);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(builder, "msi_per_vector_masking", resources->msi.per_vector_masking);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(builder, "msi_claimed", resources->msi.capability != CAP_ID_INVALID);
	if (status == SYSCALL_STATUS_OK && resources->msi.capability != CAP_ID_INVALID)
		status = add_u64(builder, "msi_message_address", resources->msi.message.message_address);
	if (status == SYSCALL_STATUS_OK && resources->msi.capability != CAP_ID_INVALID)
		status = add_u64(builder, "msi_message_data", resources->msi.message.message_data);
	if (status == SYSCALL_STATUS_OK && resources->msi.capability != CAP_ID_INVALID)
		status = device_builder_add_resource(
			builder, "msi", sizeof("msi") - 1u, resources->msi.capability, PCI_INTERRUPT_RESOURCE_RIGHTS);
	return status;
}

static syscall_status_t add_msix(const struct device_builder* builder, const struct pci_msix_resource* msix) {
	if (!msix->present) return SYSCALL_STATUS_OK;
	syscall_status_t status = add_u64(builder, "msix_capability_offset", msix->capability_offset);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msix_vector_count", msix->vector_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msix_table_bar", msix->table_bar);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msix_table_offset", msix->table_offset);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msix_pba_bar", msix->pba_bar);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "msix_pba_offset", msix->pba_offset);
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, "msix_enabled", msix->enabled);
	if (status == SYSCALL_STATUS_OK) status = add_bool(builder, "msix_function_masked", msix->function_masked);
	return status;
}

static syscall_status_t add_window(const struct device_builder* builder, const char* prefix,
                                   const struct pci_bridge_window* window) {
	char             name[48];
	syscall_status_t status;

	if (!window->present) return SYSCALL_STATUS_OK;
	int written = snprintf(name, sizeof(name), "bridge_%s_base", prefix);
	if (written <= 0 || (size_t)written >= sizeof(name)) return SYSCALL_STATUS_FAILED;
	status  = add_u64(builder, name, window->base);
	written = snprintf(name, sizeof(name), "bridge_%s_size", prefix);
	if (written <= 0 || (size_t)written >= sizeof(name)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, name, window->size);
	return status;
}

static syscall_status_t add_bridge(const struct device_builder* builder, const struct pci_bridge_resources* bridge) {
	if (!bridge->present) return SYSCALL_STATUS_OK;
	syscall_status_t status = add_u64(builder, "bridge_primary_bus", bridge->primary_bus);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "bridge_secondary_bus", bridge->secondary_bus);
	if (status == SYSCALL_STATUS_OK) status = add_u64(builder, "bridge_subordinate_bus", bridge->subordinate_bus);
	if (status == SYSCALL_STATUS_OK) status = add_window(builder, "io", &bridge->io);
	if (status == SYSCALL_STATUS_OK) status = add_window(builder, "memory", &bridge->memory);
	if (status == SYSCALL_STATUS_OK) status = add_window(builder, "prefetchable_memory", &bridge->prefetchable_memory);
	return status;
}

static syscall_status_t pci_device_publish(cap_id_t host_cap, cap_id_t memory_allocator_cap, cap_id_t dma_cap,
                                           struct pci_function* function) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char                  name[32];
	size_t                name_size;
	syscall_status_t      status;

	if (host_cap == CAP_ID_INVALID || function == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	TRACE_SCOPE(device_trace, &trace_publish_device);
	{
		TRACE_SCOPE(trace, &trace_publish_begin);
		status = device_builder_begin_child(host_cap, &builder);
	}
	if (status != SYSCALL_STATUS_OK) return status;
	if (!format(name,
	            sizeof(name),
	            &name_size,
	            "%04x:%02x:%02x.%u",
	            function->address.segment,
	            function->address.bus,
	            function->address.device,
	            function->address.function)) {
		status = SYSCALL_STATUS_FAILED;
		goto failed;
	}
	{
		TRACE_SCOPE(trace, &trace_publish_properties);
		{
			TRACE_SCOPE(step_trace, &trace_publish_set_name);
			status = device_builder_set_name(&builder, name, name_size);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_compatible);
			status = add_compatible_ids(&builder, function);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_identity);
			status = add_identity(&builder, function);
		}
	}
	if (status == SYSCALL_STATUS_OK) {
		TRACE_SCOPE(trace, &trace_publish_resources);
		{
			TRACE_SCOPE(step_trace, &trace_publish_bars);
			status = add_bars(&builder, &function->resources);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_rom);
			status = add_rom(&builder, &function->resources.rom);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_interrupts);
			status = add_interrupts(&builder, &function->resources);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_msix);
			status = add_msix(&builder, &function->resources.msix);
		}
		if (status == SYSCALL_STATUS_OK) {
			TRACE_SCOPE(step_trace, &trace_publish_bridge);
			status = add_bridge(&builder, &function->resources.bridge);
		}
	}
	if (status == SYSCALL_STATUS_OK && dma_cap != CAP_ID_INVALID) {
		TRACE_SCOPE(trace, &trace_publish_dma);
		syscall_status_t dma_status = pci_claim_dma_resource(
			&builder, dma_cap, "dma", sizeof("dma") - 1u, function->address.segment, function->resources.requester_id);
		if (dma_status == SYSCALL_STATUS_OK)
			status =
				pci_claim_dma_allocator(&builder, memory_allocator_cap, "dma_allocator", sizeof("dma_allocator") - 1u);
		else if (dma_status != SYSCALL_STATUS_UNAVAILABLE) status = dma_status;
	}
	if (status == SYSCALL_STATUS_OK) {
		TRACE_SCOPE(trace, &trace_publish_commit);
		status = device_builder_commit(&builder);
	}
	if (status == SYSCALL_STATUS_OK) {
		pci_resources_release(&function->resources);
		return SYSCALL_STATUS_OK;
	}

failed:
	(void)device_builder_abort(&builder);
	return status;
}

syscall_status_t pci_devices_publish(cap_id_t host_cap, cap_id_t memory_allocator_cap, cap_id_t dma_cap,
                                     struct pci_inventory* inventory) {
	if (host_cap == CAP_ID_INVALID || memory_allocator_cap == CAP_ID_INVALID || inventory == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	for (struct pci_function* function = inventory->functions; function != NULL; function = function->next) {
		syscall_status_t status = pci_device_publish(host_cap, memory_allocator_cap, dma_cap, function);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}
