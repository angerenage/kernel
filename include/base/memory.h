#pragma once

#include <base/cap.h>
#include <base/dma.h>
#include <stddef.h>
#include <stdint.h>

enum memory_type {
	MEMORY_TYPE_NORMAL = 0,
	MEMORY_TYPE_DEVICE,
	MEMORY_TYPE_COUNT,
};

_Static_assert(MEMORY_TYPE_COUNT < 64, "memory type mask must fit in uint64_t");

typedef uint32_t memory_access_t;

enum memory_access {
	MEMORY_ACCESS_READ  = 1u << 0,
	MEMORY_ACCESS_WRITE = 1u << 1,
	MEMORY_ACCESS_EXEC  = 1u << 2,
};

#define MEMORY_ACCESS_VALID_MASK ((memory_access_t)(MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE | MEMORY_ACCESS_EXEC))

enum memory_allocator_op {
	MEMORY_ALLOCATOR_OP_INFO = 0,
	MEMORY_ALLOCATOR_OP_DERIVE,
	MEMORY_ALLOCATOR_OP_ALLOC,
	MEMORY_ALLOCATOR_OP_CLAIM_PHYSICAL,
};

enum memory_allocator_claim_policy {
	MEMORY_ALLOCATOR_CLAIMS_NONE = 0,
	MEMORY_ALLOCATOR_CLAIMS_RESTRICTED,
	MEMORY_ALLOCATOR_CLAIMS_UNRESTRICTED,
};

enum memory_allocator_derive_flag {
	MEMORY_ALLOCATOR_DERIVE_INHERIT_CLAIMS = 1u << 0,
};

/* One physical range authorized by a MemoryAllocator policy. */
struct memory_allocator_physical_range {
	uintptr_t address;
	size_t    size;
};

/* Common header for MemoryAllocator requests. */
struct memory_allocator_request_header {
	enum memory_allocator_op op;
};

/* Request immutable MemoryAllocator policy information. */
struct memory_allocator_info_request {
	struct memory_allocator_request_header header;
};

/* Immutable public MemoryAllocator policy information. */
struct memory_allocator_info {
	cap_rights_t                       memory_rights;
	uint64_t                           memory_type_mask;
	enum memory_allocator_claim_policy claim_policy;
	size_t                             claim_range_count;
	size_t                             physical_claim_granule;
};

/* Request a policy-restricted child MemoryAllocator. */
struct memory_allocator_derive_request {
	struct memory_allocator_request_header header;
	cap_rights_t                           allocator_rights;
	cap_rights_t                           memory_rights;
	uint64_t                               memory_type_mask;
	uint32_t                               flags;
	uint32_t                               claim_range_count;
	struct memory_allocator_physical_range claim_ranges[];
};

/* Capability returned for a derived MemoryAllocator. */
struct memory_allocator_derive_response {
	cap_id_t allocator_cap;
};

/* Request sparse Memory with an exact logical byte size. */
struct memory_allocator_alloc_request {
	struct memory_allocator_request_header header;
	size_t                                 size;
};

/* Capability returned for newly allocated Memory. */
struct memory_allocator_alloc_response {
	cap_id_t memory_cap;
};

/* Request exclusive ownership of an existing physical byte range. */
struct memory_allocator_claim_physical_request {
	struct memory_allocator_request_header header;
	uintptr_t                              physical_address;
	size_t                                 size;
	enum memory_type                       memory_type;
};

/* Capability returned for claimed physical Memory. */
struct memory_allocator_claim_physical_response {
	cap_id_t memory_cap;
};

enum memory_op {
	MEMORY_OP_INFO = 0,
	MEMORY_OP_SLICE,
};

/* Common header for Memory requests. */
struct memory_request_header {
	enum memory_op op;
};

/* Request immutable Memory information. */
struct memory_info_request {
	struct memory_request_header header;
};

/* Immutable public Memory information. */
struct memory_info {
	size_t           size;
	enum memory_type memory_type;
};

/* Request a distinct child view over a Memory byte range. */
struct memory_slice_request {
	struct memory_request_header header;
	size_t                       offset;
	size_t                       size;
};

/* Capability returned for a new Memory slice. */
struct memory_slice_response {
	cap_id_t memory_cap;
};

enum mapping_op {
	MAPPING_OP_INFO = 0,
	MAPPING_OP_PROTECT,
	MAPPING_OP_UNMAP,
	MAPPING_OP_SYNC,
};

/* Common header for Mapping requests. */
struct mapping_request_header {
	enum mapping_op op;
};

/* Request immutable and current Mapping information. */
struct mapping_info_request {
	struct mapping_request_header header;
};

/* Public information describing one whole-Memory projection. */
struct mapping_info {
	uintptr_t        address;
	size_t           size;
	memory_access_t  access;
	size_t           guard_before;
	size_t           guard_after;
	enum memory_type memory_type;
};

/* Request a new protection for one Mapping. */
struct mapping_protect_request {
	struct mapping_request_header header;
	memory_access_t               access;
};

/* Request explicit destruction of one Mapping. */
struct mapping_unmap_request {
	struct mapping_request_header header;
};

/* Synchronize one DEVICE Mapping byte range for a DMA ownership transition. */
struct mapping_sync_request {
	struct mapping_request_header header;
	enum dma_sync_target          target;
	uint32_t                      reserved;
	size_t                        offset;
	size_t                        size;
};
