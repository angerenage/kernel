#include "claim.h"

#include <base/interrupt.h>
#include <base/memory.h>
#include <limits.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/interrupt.h>
#include <system/memory.h>

#if defined(__x86_64__)
#include <system/io_port.h>
#endif

#define PCI_IO_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))

static void claim_error(const struct pci_address* address, const char* resource, syscall_status_t status) {
	fprintf(stderr,
	        "pci: %04x:%02x:%02x.%u could not claim %s: %u\n",
	        address->segment,
	        address->bus,
	        address->device,
	        address->function,
	        resource,
	        (unsigned)status);
}

bool pci_claim_initialize(cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                          struct pci_claim_context* out_context) {
	struct memory_allocator_info memory;

	if (out_context == NULL || memory_allocator_cap == CAP_ID_INVALID || interrupts_cap == CAP_ID_INVALID ||
	    memory_allocator_info(memory_allocator_cap, &memory) != SYSCALL_STATUS_OK ||
	    memory.physical_claim_granule == 0u ||
	    (memory.physical_claim_granule & (memory.physical_claim_granule - 1u)) != 0u)
		return false;
	*out_context = (struct pci_claim_context){
		.memory_allocator_cap = memory_allocator_cap,
		.interrupts_cap       = interrupts_cap,
		.io_ports_cap         = io_ports_cap,
		.memory_granule       = memory.physical_claim_granule,
	};
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

#if defined(__x86_64__)
static bool claim_memory(const struct pci_claim_context* context, const struct pci_address* address, uint64_t base,
                         uint64_t size, cap_id_t* out_cap) {
	syscall_status_t status;

	if (base == 0u || size == 0u) return false;
	if (base > UINTPTR_MAX || size > SIZE_MAX || base > UINTPTR_MAX - (size - 1u) ||
	    (base & (context->memory_granule - 1u)) != 0u || size > SIZE_MAX - (context->memory_granule - 1u)) {
		claim_error(address, "unaligned MMIO range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	size_t claimed_size = ((size_t)size + context->memory_granule - 1u) & ~(context->memory_granule - 1u);
	if (claimed_size > UINTPTR_MAX - (uintptr_t)base) {
		claim_error(address, "MMIO range", SYSCALL_STATUS_BAD_ARGUMENT);
		return false;
	}
	status = memory_allocator_claim_physical(
		context->memory_allocator_cap, (uintptr_t)base, claimed_size, MEMORY_TYPE_DEVICE, out_cap);
	if (status != SYSCALL_STATUS_OK) claim_error(address, "MMIO range", status);
	return status == SYSCALL_STATUS_OK;
}

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
#if defined(__x86_64__)
	/* PCI bus addresses are identity-mapped on PC x86; other hosts still need ranges translation. */
	for (uint8_t index = 0u; index < resources->bar_count; index++) {
		struct pci_bar_resource* bar = &resources->bars[index];
		if (bar->kind == PCI_BAR_IO) count += claim_io(context, address, bar->address, bar->size, &bar->capability);
		else if (bar->kind == PCI_BAR_MEMORY32 || bar->kind == PCI_BAR_MEMORY64)
			count += claim_memory(context, address, bar->address, bar->size, &bar->capability);
	}
	if (resources->rom.present)
		count +=
			claim_memory(context, address, resources->rom.address, resources->rom.size, &resources->rom.capability);
#endif
	count += claim_msi(context, address, &resources->msi);
	return count;
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
