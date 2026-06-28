#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Operation codes for address space capability requests. */
enum address_space_op {
	ADDRESS_SPACE_OP_QUERY  = 0,
	ADDRESS_SPACE_OP_MAP    = 1,
	ADDRESS_SPACE_OP_MAP_AT = 2,
};

/* Common header for all address space capability requests. */
struct address_space_request_header {
	enum address_space_op op;
};

/* Request to query an allocation by id inside the address space. */
struct address_space_query_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
};

/* Request to map an allocation at an automatically selected address. */
struct address_space_map_request {
	struct address_space_request_header header;
	cap_id_t                            allocation_cap;
};

/* Request to map an allocation at a specific page-aligned address. */
struct address_space_map_at_request {
	struct address_space_request_header header;
	cap_id_t                            allocation_cap;
	uintptr_t                           address;
};

/* Response with metadata for an address-space allocation. */
struct address_space_query_response {
	struct vmm_info info;
};

/* Response with control over the newly-created mapping. */
struct address_space_map_response {
	cap_id_t mapping_cap;
};
