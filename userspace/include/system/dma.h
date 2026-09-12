#pragma once

#include <base/dma.h>
#include <base/syscall.h>

/* Resolve firmware/platform routing data into one opaque DMA source token. */
syscall_status_t dma_resolve_source(cap_id_t dma_cap, uint64_t controller_register_address, uint32_t local_source_id,
                                    dma_source_t* out_source);

/* Create one DEVICE AddressSpace compatible with source's IOMMU controller. */
syscall_status_t dma_create_address_space(cap_id_t dma_cap, dma_source_t source, cap_id_t* out_address_space_cap);

/* Attach source to one DEVICE AddressSpace. */
syscall_status_t dma_bind(cap_id_t dma_cap, dma_source_t source, cap_id_t address_space_cap, cap_id_t* out_binding_cap);

/* Recover capability authority for an already-active source binding. */
syscall_status_t dma_recover(cap_id_t dma_cap, dma_source_t source, cap_id_t* out_binding_cap);

/* Explicitly detach the source represented by one Binding capability. */
syscall_status_t dma_binding_unbind(cap_id_t binding_cap);
