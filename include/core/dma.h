#pragma once

#include <base/dma.h>
#include <stdbool.h>
#include <stdint.h>

struct address_space;
struct dma_binding;

/* Discover the available IOMMU controllers for DMA. */
bool dma_init(void);

/* Resolve a physical controller register address and local source ID into an opaque DMA source. */
bool dma_source_resolve(uint64_t controller_register_address, uint32_t local_source_id, dma_source_t* out_source);

/* Create a device AddressSpace compatible with one DMA source. */
bool dma_address_space_create(dma_source_t compatibility_source, struct address_space** out_space);

/* Attach a DMA source to a device AddressSpace. */
bool dma_bind(dma_source_t source, struct address_space* device_space, struct dma_binding** out_binding);

/* Detach a DMA source from its device AddressSpace. */
bool dma_unbind(struct dma_binding* binding);

/* Recover an active binding for a DMA source. */
bool dma_binding_recover(dma_source_t source, struct dma_binding** out_binding);

/* Retain a DMA binding. */
bool dma_binding_retain(struct dma_binding* binding);

/* Release a DMA binding reference. */
void dma_binding_release(struct dma_binding* binding);

/* Return the source attached by a DMA binding. */
dma_source_t dma_binding_source(const struct dma_binding* binding);

/* Return a retained DEVICE AddressSpace while the DMA binding remains attached.
 * Release it with address_space_device_release(). */
struct address_space* dma_binding_address_space(struct dma_binding* binding);

/* Return whether a DMA binding remains attached. */
bool dma_binding_is_active(const struct dma_binding* binding);
