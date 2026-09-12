#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

enum dma_sync_target {
	DMA_SYNC_FOR_DEVICE = 0,
	DMA_SYNC_FOR_CPU,
};

typedef uint64_t dma_source_t;

#define DMA_SOURCE_INVALID UINT64_MAX

enum dma_op {
	DMA_OP_RESOLVE_SOURCE = 0,
	DMA_OP_CREATE_ADDRESS_SPACE,
	DMA_OP_BIND,
	DMA_OP_RECOVER,
};

struct dma_request_header {
	enum dma_op op;
};

struct dma_resolve_source_request {
	struct dma_request_header header;
	uint64_t                  controller_register_address;
	uint32_t                  local_source_id;
	uint32_t                  reserved;
};

struct dma_resolve_source_response {
	dma_source_t source;
};

struct dma_create_address_space_request {
	struct dma_request_header header;
	dma_source_t              source;
};

struct dma_create_address_space_response {
	cap_id_t address_space_cap;
};

struct dma_bind_request {
	struct dma_request_header header;
	dma_source_t              source;
	cap_id_t                  address_space_cap;
};

struct dma_bind_response {
	cap_id_t binding_cap;
};

struct dma_recover_request {
	struct dma_request_header header;
	dma_source_t              source;
};

struct dma_recover_response {
	cap_id_t binding_cap;
};

enum dma_binding_op {
	DMA_BINDING_OP_UNBIND = 0,
};

struct dma_binding_request_header {
	enum dma_binding_op op;
};

struct dma_binding_unbind_request {
	struct dma_binding_request_header header;
};
