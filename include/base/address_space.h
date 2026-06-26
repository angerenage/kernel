#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Operation codes for address space capability requests. */
enum address_space_op {
	ADDRESS_SPACE_OP_RESERVE    = 0,
	ADDRESS_SPACE_OP_RESERVE_AT = 1,
	ADDRESS_SPACE_OP_FREE       = 2,
	ADDRESS_SPACE_OP_MAP        = 3,
	ADDRESS_SPACE_OP_UNMAP      = 4,
	ADDRESS_SPACE_OP_PROTECT    = 5,
	ADDRESS_SPACE_OP_QUERY      = 6,
	ADDRESS_SPACE_OP_COPY_FROM  = 7,
	ADDRESS_SPACE_OP_COPY_TO    = 8,
};

/* Common header for all address space capability requests. */
struct address_space_request_header {
	enum address_space_op op;
};

/* Request to reserve virtual memory. */
struct address_space_reserve_request {
	struct address_space_request_header header;
	size_t                              page_count;
	vmm_prot_t                          prot;
	enum vmm_kind                       kind;
	uint64_t                            map_flags;
};

/* Request to reserve virtual memory at a specific address. */
struct address_space_reserve_at_request {
	struct address_space_request_header header;
	uintptr_t                           address;
	size_t                              page_count;
	vmm_prot_t                          prot;
	enum vmm_kind                       kind;
	uint64_t                            map_flags;
};

/* Request to free virtual memory. */
struct address_space_free_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
};

/* Request to map virtual memory. */
struct address_space_map_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
};

/* Request to unmap virtual memory. */
struct address_space_unmap_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
	bool                                free_pages;
};

/* Request to change protection of virtual memory. */
struct address_space_protect_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
	vmm_prot_t                          prot;
};

/* Request to query virtual memory information. */
struct address_space_query_request {
	struct address_space_request_header header;
	vmm_id_t                            id;
};

/* Request to copy data from another address space. */
struct address_space_copy_from_request {
	struct address_space_request_header header;
	uintptr_t                           src_address;
	uintptr_t                           dst_address;
	size_t                              size;
	process_id_t                        src_process_id;
};

/* Request to copy data to another address space. */
struct address_space_copy_to_request {
	struct address_space_request_header header;
	uintptr_t                           src_address;
	uintptr_t                           dst_address;
	size_t                              size;
	process_id_t                        dst_process_id;
};

/* Response with the result of a reserve operation. */
struct address_space_reserve_response {
	vmm_id_t id;
	void*    base;
};

/* Response with the result of a query operation. */
struct address_space_query_response {
	struct vmm_info info;
};
