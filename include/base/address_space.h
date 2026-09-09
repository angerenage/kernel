#pragma once

#include <base/cap.h>
#include <base/memory.h>
#include <stddef.h>
#include <stdint.h>

enum address_space_op {
	ADDRESS_SPACE_OP_INFO = 0,
	ADDRESS_SPACE_OP_MAP,
};

enum address_space_kind {
	ADDRESS_SPACE_KIND_PROCESS = 0,
};

/* Common header for AddressSpace requests. */
struct address_space_request_header {
	enum address_space_op op;
};

/* Request immutable AddressSpace information. */
struct address_space_info_request {
	struct address_space_request_header header;
};

/* Public geometry and kind of one AddressSpace. */
struct address_space_info {
	enum address_space_kind kind;
	uintptr_t               minimum_address;
	uintptr_t               maximum_address;
	size_t                  minimum_mapping_size;
};

/* Request projection of one complete Memory. */
struct address_space_map_request {
	struct address_space_request_header header;
	cap_id_t                            memory_cap;
	memory_access_t                     access;
	uintptr_t                           address;
	size_t                              alignment;
	size_t                              guard_before;
	size_t                              guard_after;
};

/* Capability and virtual address returned for a new Mapping. */
struct address_space_map_response {
	cap_id_t  mapping_cap;
	uintptr_t address;
};
