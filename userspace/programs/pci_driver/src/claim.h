#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "ranges.h"
#include "resource.h"

struct device_builder;

/* Root authorities and cached geometry used while claiming PCI resources. */
struct pci_claim_context {
	cap_id_t                      memory_allocator_cap;
	cap_id_t                      interrupts_cap;
	cap_id_t                      io_ports_cap;
	size_t                        memory_granule;
	const struct pci_host_ranges* ranges;
	cap_id_t                      host_memory_caps[PCI_HOST_RANGE_COUNT];
#if defined(__x86_64__)
	uint32_t io_base;
	uint32_t io_count;
#endif
};

/* Validate the configured root capabilities and cache their public geometry. */
bool pci_claim_initialize(const struct pci_host_ranges* ranges, cap_id_t memory_allocator_cap, cap_id_t interrupts_cap,
                          cap_id_t io_ports_cap, struct pci_claim_context* out_context);

/* Release the host-window capabilities retained by a claim context. */
void pci_claim_deinitialize(struct pci_claim_context* context);

/* Claim every currently supported, assigned resource of one PCI function. */
size_t pci_resources_claim(const struct pci_claim_context* context, const struct pci_address* address,
                           struct pci_resources* resources);

/* Claim one PCI requester's DMA source and attach it to a device being published. */
syscall_status_t pci_claim_dma_resource(const struct device_builder* builder, cap_id_t dma_cap, const char* name,
                                        size_t name_size, uint16_t segment, uint16_t requester_id);

/* Release all claims retained in one resource description. */
void pci_resources_release(struct pci_resources* resources);
