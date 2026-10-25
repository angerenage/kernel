#include "dma.h"

#include <base/address_space.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/dma.h>
#include <system/memory.h>
#include <system/process.h>

#define AHCI_DMA_RESOURCE "dma"
#define AHCI_DMA_ALLOCATOR_RESOURCE "dma_allocator"
#define AHCI_DMA_SOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_ALLOCATE | CAP_MANAGE))
#define AHCI_DMA_ALLOCATOR_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_ALLOCATE))
#define AHCI_DMA_MEMORY_RIGHTS ((cap_rights_t)(CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))

static bool align_up(size_t value, size_t alignment, size_t* out_value) {
	if (out_value == NULL || alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
	    value > SIZE_MAX - (alignment - 1u))
		return false;
	*out_value = (value + alignment - 1u) & ~(alignment - 1u);
	return true;
}

void ahci_dma_deinitialize(struct ahci_dma_context* context) {
	if (context == NULL) return;
	if (context->binding_cap != CAP_ID_INVALID) (void)dma_binding_unbind(context->binding_cap);
	if (context->device_space_cap != CAP_ID_INVALID) (void)cap_drop(context->device_space_cap);
	if (context->process_space_cap != CAP_ID_INVALID) (void)cap_drop(context->process_space_cap);
	if (context->memory_allocator_cap != CAP_ID_INVALID) (void)cap_drop(context->memory_allocator_cap);
	if (context->source_cap != CAP_ID_INVALID) (void)cap_drop(context->source_cap);
	*context = (struct ahci_dma_context){
		.source_cap           = CAP_ID_INVALID,
		.memory_allocator_cap = CAP_ID_INVALID,
		.device_space_cap     = CAP_ID_INVALID,
		.binding_cap          = CAP_ID_INVALID,
		.process_space_cap    = CAP_ID_INVALID,
	};
}

bool ahci_dma_initialize(cap_id_t device_cap, struct ahci_dma_context* out_context) {
	struct address_space_info    device_space;
	struct memory_allocator_info allocator;
	struct self_info             self;
	syscall_status_t             status;

	if (out_context == NULL) return false;
	*out_context = (struct ahci_dma_context){
		.source_cap           = CAP_ID_INVALID,
		.memory_allocator_cap = CAP_ID_INVALID,
		.device_space_cap     = CAP_ID_INVALID,
		.binding_cap          = CAP_ID_INVALID,
		.process_space_cap    = CAP_ID_INVALID,
	};
	status = device_resource_acquire(device_cap,
	                                 AHCI_DMA_ALLOCATOR_RESOURCE,
	                                 sizeof(AHCI_DMA_ALLOCATOR_RESOURCE) - 1u,
	                                 AHCI_DMA_ALLOCATOR_RIGHTS,
	                                 &out_context->memory_allocator_cap);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "ahci: unable to acquire DMA memory allocator: %u\n", (unsigned)status);
		goto fail;
	}
	status = memory_allocator_info(out_context->memory_allocator_cap, &allocator);
	if (status != SYSCALL_STATUS_OK || allocator.memory_rights != AHCI_DMA_MEMORY_RIGHTS ||
	    allocator.memory_type_mask != (1ull << MEMORY_TYPE_NORMAL) ||
	    allocator.claim_policy != MEMORY_ALLOCATOR_CLAIMS_NONE || allocator.claim_range_count != 0u) {
		fprintf(stderr, "ahci: invalid DMA memory allocator policy: %u\n", (unsigned)status);
		goto fail;
	}
	status = device_resource_acquire(device_cap,
	                                 AHCI_DMA_RESOURCE,
	                                 sizeof(AHCI_DMA_RESOURCE) - 1u,
	                                 AHCI_DMA_SOURCE_RIGHTS,
	                                 &out_context->source_cap);
	if (status != SYSCALL_STATUS_OK) {
		if (status == SYSCALL_STATUS_UNAVAILABLE)
			fprintf(stderr, "ahci: no DMA source was published for this PCI function\n");
		else fprintf(stderr, "ahci: unable to acquire DMA source resource: %u\n", (unsigned)status);
		goto fail;
	}
	status = dma_source_create_address_space(out_context->source_cap, &out_context->device_space_cap);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "ahci: unable to create DMA address space: %u\n", (unsigned)status);
		goto fail;
	}
	status = address_space_info(out_context->device_space_cap, &device_space);
	if (status != SYSCALL_STATUS_OK || device_space.kind != ADDRESS_SPACE_KIND_DEVICE ||
	    device_space.minimum_mapping_size == 0u ||
	    (device_space.minimum_mapping_size & (device_space.minimum_mapping_size - 1u)) != 0u) {
		fprintf(stderr, "ahci: invalid DMA address-space geometry: %u\n", (unsigned)status);
		goto fail;
	}
	status = dma_source_bind(out_context->source_cap, out_context->device_space_cap, &out_context->binding_cap);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "ahci: unable to bind DMA source: %u\n", (unsigned)status);
		goto fail;
	}
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "ahci: unable to acquire the process address space: %u\n", (unsigned)status);
		goto fail;
	}
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.address_space_cap == CAP_ID_INVALID) goto fail;
	out_context->process_space_cap = self.address_space_cap;
	out_context->mapping_granule   = device_space.minimum_mapping_size;
	return true;

fail:
	ahci_dma_deinitialize(out_context);
	return false;
}

void ahci_dma_buffer_release(struct ahci_dma_buffer* buffer) {
	if (buffer == NULL) return;
	if (buffer->device_mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(buffer->device_mapping_cap);
	if (buffer->cpu_mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(buffer->cpu_mapping_cap);
	if (buffer->memory_cap != CAP_ID_INVALID) (void)cap_drop(buffer->memory_cap);
	*buffer = (struct ahci_dma_buffer){
		.memory_cap         = CAP_ID_INVALID,
		.cpu_mapping_cap    = CAP_ID_INVALID,
		.device_mapping_cap = CAP_ID_INVALID,
	};
}

bool ahci_dma_buffer_allocate(const struct ahci_dma_context* context, size_t size, memory_access_t device_access,
                              struct ahci_dma_buffer* out_buffer) {
	cap_id_t                          memory_cap = CAP_ID_INVALID;
	struct address_space_info         process_space;
	struct address_space_map_response cpu_mapping    = {.mapping_cap = CAP_ID_INVALID};
	struct address_space_map_response device_mapping = {.mapping_cap = CAP_ID_INVALID};
	size_t                            alignment;
	size_t                            allocation_size;

	if (context == NULL || out_buffer == NULL || context->device_space_cap == CAP_ID_INVALID ||
	    context->process_space_cap == CAP_ID_INVALID || context->mapping_granule == 0u ||
	    context->memory_allocator_cap == CAP_ID_INVALID || size == 0u || device_access == 0u ||
	    (device_access & ~MEMORY_ACCESS_VALID_MASK) != 0u || (device_access & MEMORY_ACCESS_EXEC) != 0u)
		return false;
	*out_buffer = (struct ahci_dma_buffer){
		.memory_cap         = CAP_ID_INVALID,
		.cpu_mapping_cap    = CAP_ID_INVALID,
		.device_mapping_cap = CAP_ID_INVALID,
	};
	if (address_space_info(context->process_space_cap, &process_space) != SYSCALL_STATUS_OK ||
	    process_space.kind != ADDRESS_SPACE_KIND_PROCESS || process_space.minimum_mapping_size == 0u ||
	    (process_space.minimum_mapping_size & (process_space.minimum_mapping_size - 1u)) != 0u)
		return false;
	alignment = context->mapping_granule > process_space.minimum_mapping_size ? context->mapping_granule
	                                                                          : process_space.minimum_mapping_size;
	if (!align_up(size, alignment, &allocation_size) ||
	    memory_allocator_alloc(context->memory_allocator_cap, allocation_size, &memory_cap) != SYSCALL_STATUS_OK ||
	    address_space_map(context->process_space_cap,
	                      memory_cap,
	                      MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                      0u,
	                      alignment,
	                      0u,
	                      0u,
	                      &cpu_mapping) != SYSCALL_STATUS_OK ||
	    cpu_mapping.mapping_cap == CAP_ID_INVALID || cpu_mapping.address == 0u ||
	    address_space_map(context->device_space_cap,
	                      memory_cap,
	                      device_access,
	                      0u,
	                      context->mapping_granule,
	                      0u,
	                      0u,
	                      &device_mapping) != SYSCALL_STATUS_OK ||
	    device_mapping.mapping_cap == CAP_ID_INVALID || device_mapping.address == 0u)
		goto fail;
	memset((void*)cpu_mapping.address, 0, allocation_size);
	if (mapping_sync(device_mapping.mapping_cap, 0u, allocation_size, DMA_SYNC_FOR_DEVICE) != SYSCALL_STATUS_OK)
		goto fail;
	*out_buffer = (struct ahci_dma_buffer){
		.memory_cap         = memory_cap,
		.cpu_mapping_cap    = cpu_mapping.mapping_cap,
		.device_mapping_cap = device_mapping.mapping_cap,
		.cpu_address        = (void*)cpu_mapping.address,
		.device_address     = device_mapping.address,
		.size               = allocation_size,
	};
	memory_cap = CAP_ID_INVALID;
	return true;

fail:
	if (device_mapping.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(device_mapping.mapping_cap);
	if (cpu_mapping.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(cpu_mapping.mapping_cap);
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return false;
}
