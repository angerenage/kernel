#pragma once

#include <base/cap.h>
#include <base/vmm.h>
#include <stddef.h>
#include <stdint.h>

/* Operation codes for allocation capability requests. */
enum allocation_op {
	ALLOCATION_OP_FREE      = 4,
	ALLOCATION_OP_READ      = 5,
	ALLOCATION_OP_COPY_FROM = 6,
	ALLOCATION_OP_COPY_TO   = 7,
};

/* Operation codes for mapping capability requests. */
enum mapping_op {
	MAPPING_OP_READ    = 0,
	MAPPING_OP_PROTECT = 1,
	MAPPING_OP_UNMAP   = 2,
};

/* Common header for all allocation capability requests. */
struct allocation_request_header {
	enum allocation_op op;
};

/* Common header for all mapping capability requests. */
struct mapping_request_header {
	enum mapping_op op;
};

/* Request to free an allocation. */
struct allocation_free_request {
	struct allocation_request_header header;
};

/* Request to read an allocation's metadata. */
struct allocation_read_request {
	struct allocation_request_header header;
};

/* Request to copy data out of an allocation into the caller's address space. */
struct allocation_copy_from_request {
	struct allocation_request_header header;
	uintptr_t                        src_offset;
	uintptr_t                        dst_address;
	size_t                           size;
};

/* Request to copy data from the caller's address space into an allocation. */
struct allocation_copy_to_request {
	struct allocation_request_header header;
	uintptr_t                        src_address;
	uintptr_t                        dst_offset;
	size_t                           size;
};

/* Response with the number of bytes copied. */
struct allocation_copy_response {
	size_t bytes_copied;
};

/* Response with allocation metadata. */
struct allocation_read_response {
	struct vmm_info info;
};

/* Request to read one mapping's metadata. */
struct mapping_read_request {
	struct mapping_request_header header;
};

/* Request to change one mapping's protection bits. */
struct mapping_protect_request {
	struct mapping_request_header header;
	vmm_prot_t                    prot;
};

/* Request to destroy one mapping. */
struct mapping_unmap_request {
	struct mapping_request_header header;
};

/* Response with metadata for one mapping. */
struct mapping_read_response {
	struct vmm_info info;
};

/* Request to create a new memory allocation. */
struct syscall_allocate_request {
	size_t     page_count;
	vmm_prot_t prot;
};

/* Response with a new allocation capability. */
struct syscall_allocate_response {
	cap_id_t allocation_cap;
};
