#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Direction in which a DMA mapping must be synchronized. */
enum dma_sync_target {
	DMA_SYNC_FOR_DEVICE = 0,
	DMA_SYNC_FOR_CPU,
};

/* Opaque, non-authoritative token naming one IOMMU input source. */
typedef uint64_t dma_source_t;

#define DMA_SOURCE_INVALID UINT64_MAX

/* Operations accepted by the root DMA resource. */
enum dma_op {
	DMA_OP_RESOLVE_SOURCE = 0,
	DMA_OP_CLAIM_SOURCE,
};

/* Common header for root DMA-resource requests. */
struct dma_request_header {
	enum dma_op op;
};

/* Request to resolve firmware routing data into an opaque source token. */
struct dma_resolve_source_request {
	struct dma_request_header header;
	uint64_t                  controller_register_address;
	uint32_t                  local_source_id;
	uint32_t                  reserved;
};

/* Response returned after resolving one DMA source. */
struct dma_resolve_source_response {
	dma_source_t source;
};

/* Request to claim exclusive authority over one resolved DMA source. */
struct dma_claim_source_request {
	struct dma_request_header header;
	dma_source_t              source;
};

/* Response returned after claiming one DMA source. */
struct dma_claim_source_response {
	cap_id_t source_cap;
};

/* Operations accepted by a claimed DMA-source capability. */
enum dma_source_op {
	DMA_SOURCE_OP_CREATE_ADDRESS_SPACE = 0,
	DMA_SOURCE_OP_BIND,
	DMA_SOURCE_OP_RECOVER,
};

/* Common header for claimed DMA-source requests. */
struct dma_source_request_header {
	enum dma_source_op op;
};

/* Request to create a DEVICE AddressSpace compatible with the claimed source. */
struct dma_source_create_address_space_request {
	struct dma_source_request_header header;
};

/* Response returned after creating a DEVICE AddressSpace. */
struct dma_source_create_address_space_response {
	cap_id_t address_space_cap;
};

/* Request to attach the claimed source to a DEVICE AddressSpace. */
struct dma_source_bind_request {
	struct dma_source_request_header header;
	cap_id_t                         address_space_cap;
};

/* Response returned after attaching a claimed source. */
struct dma_source_bind_response {
	cap_id_t binding_cap;
};

/* Empty request used to recover the claimed source's active binding. */
struct dma_source_recover_request {
	struct dma_source_request_header header;
};

/* Response returned when recovering an active source binding. */
struct dma_source_recover_response {
	cap_id_t binding_cap;
};

/* Operations accepted by a DMA Binding capability. */
enum dma_binding_op {
	DMA_BINDING_OP_UNBIND = 0,
};

/* Common header for DMA Binding requests. */
struct dma_binding_request_header {
	enum dma_binding_op op;
};

/* Request to detach the source represented by a DMA Binding capability. */
struct dma_binding_unbind_request {
	struct dma_binding_request_header header;
};
