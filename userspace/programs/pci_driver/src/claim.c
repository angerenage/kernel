#include "claim.h"

#include <base/dma.h>
#include <base/interrupt.h>
#include <base/memory.h>
#include <limits.h>
#include <runtime/device.h>
#include <runtime/log.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/dma.h>
#include <system/interrupt.h>
#include <system/memory.h>

#if defined(__x86_64__)
#include <system/io_port.h>
#endif

#define PCI_IO_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))
#define PCI_DMA_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_ALLOCATE | CAP_MANAGE))
#define PCI_DMA_ALLOCATOR_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_ALLOCATE))

static void claim_error(const struct pci_address* address, const char* resource, syscall_status_t status) {
	RUNTIME_DEBUG_ERROR("pci: %04x:%02x:%02x.%u could not claim %s: %u\n",
	                    address->segment,
	                    address->bus,
	                    address->device,
	                    address->function,
	                    resource,
	                    (unsigned)status);
}

bool pci_claim_initialize(const struct pci_host_ranges* ranges, cap_id_t memory_allocator_cap, cap_id_t interrupts_cap,
                          cap_id_t io_ports_cap, struct pci_claim_context* out_context) {
	struct memory_allocator_info memory;

	if (ranges == NULL || out_context == NULL || memory_allocator_cap == CAP_ID_INVALID ||
	    interrupts_cap == CAP_ID_INVALID || memory_allocator_info(memory_allocator_cap, &memory) != SYSCALL_STATUS_OK ||
	    memory.physical_claim_granule == 0u ||
	    (memory.physical_claim_granule & (memory.physical_claim_granule - 1u)) != 0u)
		return false;
	*out_context = (struct pci_claim_context){
		.memory_allocator_cap = memory_allocator_cap,
		.interrupts_cap       = interrupts_cap,
		.io_ports_cap         = io_ports_cap,
		.memory_granule       = memory.physical_claim_granule,
		.ranges               = ranges,
	};
#if !defined(__x86_64__)
	for (size_t index = 0u; index < ranges->count; index++) {
		const struct pci_host_range* range = &ranges->values[index];
		syscall_status_t             status;

		if (range->cpu_address > UINTPTR_MAX || range->size > SIZE_MAX ||
		    (range->cpu_address & (memory.physical_claim_granule - 1u)) != 0u ||
		    (range->size & (memory.physical_claim_granule - 1u)) != 0u)
			continue;
		status = memory_allocator_claim_physical(memory_allocator_cap,
		                                         (uintptr_t)range->cpu_address,
		                                         (size_t)range->size,
		                                         MEMORY_TYPE_DEVICE,
		                                         &out_context->host_memory_caps[index]);
		if (status != SYSCALL_STATUS_OK)
			RUNTIME_DEBUG_ERROR("pci: could not claim Device Tree host range %zu: %u\n", index, (unsigned)status);
	}
#endif
#if defined(__x86_64__)
	if (io_ports_cap != CAP_ID_INVALID) {
		struct io_port_info ports;
		if (io_port_info(io_ports_cap, &ports) != SYSCALL_STATUS_OK) return false;
		out_context->io_base  = ports.base;
		out_context->io_count = ports.count;
	}
#else
	(void)io_ports_cap;
#endif
	return true;
}

static bool claim_memory(const struct pci_claim_context* context, const struct pci_address* address, uint64_t base,
                         uint64_t size, cap_id_t* out_cap, uint64_t* out_mapping_offset) {
	syscall_status_t status;
	size_t           mapping_offset;
	uintptr_t        claim_base;
	size_t           required_size;
	size_t           claimed_size;

	if (base == 0u || size == 0u || out_mapping_offset == NULL) return false;
	*out_mapping_offset = 0u;
	if (base > UINTPTR_MAX || size > SIZE_MAX || base > UINTPTR_MAX - (size - 1u)) {
		claim_error(address, "MMIO range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	mapping_offset = (size_t)base & (context->memory_granule - 1u);
	claim_base     = (uintptr_t)base - mapping_offset;
	if ((size_t)size > SIZE_MAX - mapping_offset) {
		claim_error(address, "MMIO range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	required_size = mapping_offset + (size_t)size;
	if (required_size > SIZE_MAX - (context->memory_granule - 1u)) {
		claim_error(address, "MMIO range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	claimed_size = (required_size + context->memory_granule - 1u) & ~(context->memory_granule - 1u);
	if (claimed_size > UINTPTR_MAX - claim_base) return false;
	status = memory_allocator_claim_physical(
		context->memory_allocator_cap, claim_base, claimed_size, MEMORY_TYPE_DEVICE, out_cap);
	if (status != SYSCALL_STATUS_OK) claim_error(address, "MMIO range", status);
	else *out_mapping_offset = mapping_offset;
	return status == SYSCALL_STATUS_OK;
}

#if defined(__x86_64__)
static bool claim_io(const struct pci_claim_context* context, const struct pci_address* address, uint64_t base,
                     uint64_t size, cap_id_t* out_cap) {
	syscall_status_t status;
	uint64_t         limit;

	if (base == 0u || size == 0u || context->io_ports_cap == CAP_ID_INVALID) return false;
	limit = (uint64_t)context->io_base + context->io_count;
	if (base < context->io_base || base >= limit || size > limit - base || size > UINT32_MAX) {
		claim_error(address, "I/O range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	status = io_port_derive(
		context->io_ports_cap, (uint32_t)(base - context->io_base), (uint32_t)size, PCI_IO_RESOURCE_RIGHTS, out_cap);
	if (status != SYSCALL_STATUS_OK) claim_error(address, "I/O range", status);
	return status == SYSCALL_STATUS_OK;
}
#endif

static bool claim_host_memory(const struct pci_claim_context* context, const struct pci_address* address,
                              enum pci_host_range_kind kind, uint64_t bus_address, uint64_t size, cap_id_t* out_cap,
                              uint64_t* out_mapping_offset) {
	size_t   range_index;
	uint64_t offset;
	uint64_t cpu_address;

	if (out_mapping_offset == NULL) return false;
	*out_mapping_offset = 0u;
	if (!pci_host_ranges_locate(context->ranges, kind, bus_address, size, &range_index, &offset, &cpu_address)) {
		claim_error(address, "untranslated host range", SYSCALL_STATUS_UNAVAILABLE);
		return false;
	}
	if (context->ranges->identity)
		return claim_memory(context, address, cpu_address, size, out_cap, out_mapping_offset);
#if !defined(__x86_64__)
	if (range_index >= PCI_HOST_RANGE_COUNT || context->host_memory_caps[range_index] == CAP_ID_INVALID ||
	    offset > SIZE_MAX || size > SIZE_MAX) {
		claim_error(address, "host range", SYSCALL_STATUS_UNAVAILABLE);
		return false;
	}
	size_t mapping_offset = (size_t)offset & (context->memory_granule - 1u);
	size_t slice_offset   = (size_t)offset - mapping_offset;
	if ((size_t)size > SIZE_MAX - mapping_offset) {
		claim_error(address, "host range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	size_t required = mapping_offset + (size_t)size;
	if (required > SIZE_MAX - (context->memory_granule - 1u)) {
		claim_error(address, "host range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	size_t           slice_size = (required + context->memory_granule - 1u) & ~(context->memory_granule - 1u);
	syscall_status_t status = memory_slice(context->host_memory_caps[range_index], slice_offset, slice_size, out_cap);
	if (status != SYSCALL_STATUS_OK) claim_error(address, "host range", status);
	else *out_mapping_offset = mapping_offset;
	return status == SYSCALL_STATUS_OK;
#else
	return false;
#endif
}

static bool claim_msi(const struct pci_claim_context* context, const struct pci_address* address,
                      struct pci_msi_resource* msi) {
	interrupt_message_context_t message_context;
	syscall_status_t            status;

	if (!msi->present) return false;
	status = interrupts_resolve_message_context(
		context->interrupts_cap, INTERRUPT_MESSAGE_CONTROLLER_AUTO, INTERRUPT_MESSAGE_PRODUCER_NONE, &message_context);
	if (status == SYSCALL_STATUS_OK)
		status = interrupts_allocate_message(context->interrupts_cap, message_context, &msi->capability, &msi->message);
	if (status != SYSCALL_STATUS_OK) claim_error(address, "MSI", status);
	return status == SYSCALL_STATUS_OK;
}

size_t pci_resources_claim(const struct pci_claim_context* context, const struct pci_address* address,
                           struct pci_resources* resources) {
	size_t count = 0u;

	if (context == NULL || address == NULL || resources == NULL) return 0u;
	for (uint8_t index = 0u; index < resources->bar_count; index++) {
		struct pci_bar_resource* bar = &resources->bars[index];
#if defined(__x86_64__)
		if (bar->kind == PCI_BAR_IO) count += claim_io(context, address, bar->address, bar->size, &bar->capability);
#else
		if (bar->kind == PCI_BAR_IO && !context->ranges->identity)
			count += claim_host_memory(
				context, address, PCI_HOST_RANGE_IO, bar->address, bar->size, &bar->capability, &bar->mapping_offset);
#endif
		else if (bar->kind == PCI_BAR_MEMORY32 || bar->kind == PCI_BAR_MEMORY64)
			count += claim_host_memory(context,
			                           address,
			                           PCI_HOST_RANGE_MEMORY,
			                           bar->address,
			                           bar->size,
			                           &bar->capability,
			                           &bar->mapping_offset);
	}
	if (resources->rom.present && resources->rom.address != 0u)
		count += claim_host_memory(context,
		                           address,
		                           PCI_HOST_RANGE_MEMORY,
		                           resources->rom.address,
		                           resources->rom.size,
		                           &resources->rom.capability,
		                           &resources->rom.mapping_offset);
	count += claim_msi(context, address, &resources->msi);
	return count;
}

syscall_status_t pci_claim_dma_resource(const struct device_builder* builder, cap_id_t dma_cap, const char* name,
                                        size_t name_size, uint16_t segment, uint16_t requester_id) {
	cap_id_t         source_cap = CAP_ID_INVALID;
	syscall_status_t status;

	if (builder == NULL || dma_cap == CAP_ID_INVALID || name == NULL || name_size == 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = dma_claim_pci_source(dma_cap, segment, requester_id, &source_cap);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(builder, name, name_size, source_cap, PCI_DMA_RESOURCE_RIGHTS);
	if (source_cap != CAP_ID_INVALID) {
		syscall_status_t cleanup_status = cap_drop(source_cap);
		if (status == SYSCALL_STATUS_OK) status = cleanup_status;
	}
	return status;
}

syscall_status_t pci_claim_dma_allocator(const struct device_builder* builder, cap_id_t memory_allocator_cap,
                                         const char* name, size_t name_size) {
	const struct memory_allocator_derive_request request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_DERIVE},
		.allocator_rights = CAP_CALL | CAP_READ | CAP_ALLOCATE | CAP_DELEGATE,
		.memory_rights    = CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE,
		.memory_type_mask = 1ull << MEMORY_TYPE_NORMAL,
	};
	cap_id_t         allocator_cap = CAP_ID_INVALID;
	syscall_status_t status;

	if (builder == NULL || memory_allocator_cap == CAP_ID_INVALID || name == NULL || name_size == 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = memory_allocator_derive(memory_allocator_cap, &request, sizeof(request), &allocator_cap);
	if (status == SYSCALL_STATUS_OK)
		status =
			device_builder_add_resource(builder, name, name_size, allocator_cap, PCI_DMA_ALLOCATOR_RESOURCE_RIGHTS);
	if (allocator_cap != CAP_ID_INVALID) {
		syscall_status_t cleanup_status = cap_drop(allocator_cap);
		if (status == SYSCALL_STATUS_OK) status = cleanup_status;
	}
	return status;
}

void pci_claim_deinitialize(struct pci_claim_context* context) {
	if (context == NULL) return;
	for (size_t index = 0u; index < PCI_HOST_RANGE_COUNT; index++)
		if (context->host_memory_caps[index] != CAP_ID_INVALID) (void)cap_drop(context->host_memory_caps[index]);
	*context = (struct pci_claim_context){0};
}

void pci_resources_release(struct pci_resources* resources) {
	if (resources == NULL) return;
	for (uint8_t index = 0u; index < resources->bar_count; index++) {
		if (resources->bars[index].capability != CAP_ID_INVALID) (void)cap_drop(resources->bars[index].capability);
		resources->bars[index].capability = CAP_ID_INVALID;
	}
	if (resources->rom.capability != CAP_ID_INVALID) (void)cap_drop(resources->rom.capability);
	resources->rom.capability = CAP_ID_INVALID;
	if (resources->msi.capability != CAP_ID_INVALID) (void)cap_drop(resources->msi.capability);
	resources->msi.capability = CAP_ID_INVALID;
}
