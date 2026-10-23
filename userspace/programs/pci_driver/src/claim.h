#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "resource.h"

/* Root authorities and cached geometry used while claiming PCI resources. */
struct pci_claim_context {
	cap_id_t memory_allocator_cap;
	cap_id_t interrupts_cap;
	cap_id_t io_ports_cap;
	size_t   memory_granule;
#if defined(__x86_64__)
	uint32_t io_base;
	uint32_t io_count;
#endif
};

/* Validate the configured root capabilities and cache their public geometry. */
bool pci_claim_initialize(cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                          struct pci_claim_context* out_context);

/* Claim every currently supported, assigned resource of one PCI function. */
size_t pci_resources_claim(const struct pci_claim_context* context, const struct pci_address* address,
                           struct pci_resources* resources);

/* Release all claims retained in one resource description. */
void pci_resources_release(struct pci_resources* resources);
