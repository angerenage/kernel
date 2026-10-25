#pragma once

#include <base/cap.h>
#include <base/memory.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Translated or direct DMA context exclusively bound to one AHCI PCI function. */
struct ahci_dma_context {
	cap_id_t source_cap;
	cap_id_t memory_allocator_cap;
	cap_id_t device_space_cap;
	cap_id_t binding_cap;
	cap_id_t process_space_cap;
	size_t   mapping_granule;
};

/* One allocation projected into both CPU and device address spaces. */
struct ahci_dma_buffer {
	cap_id_t  cpu_mapping_cap;
	cap_id_t  device_mapping_cap;
	void*     cpu_address;
	uintptr_t device_address;
	size_t    size;
};

/* Acquire the device's DMA source and allocator, then bind a fresh device address space. */
bool ahci_dma_initialize(cap_id_t device_cap, struct ahci_dma_context* out_context);

/* Allocate zeroed DMA memory visible to both the driver and controller. */
bool ahci_dma_buffer_allocate(const struct ahci_dma_context* context, size_t size, memory_access_t device_access,
                              struct ahci_dma_buffer* out_buffer);

/* Destroy both projections of a DMA allocation. */
void ahci_dma_buffer_release(struct ahci_dma_buffer* buffer);

/* Unbind and release a DMA context after all buffers have been released. */
void ahci_dma_deinitialize(struct ahci_dma_context* context);
