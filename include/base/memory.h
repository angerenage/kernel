#pragma once

#include <base/vmm.h>
#include <stddef.h>
#include <stdint.h>

enum memory_constraint_flag {
	MEMORY_CONSTRAINT_CONTIGUOUS = 1u << 0,
	MEMORY_CONSTRAINT_FIXED      = 1u << 1,
};

/* Physical placement requirements for a memory object. */
struct memory_constraints {
	uintptr_t physical_min;
	uintptr_t physical_max;
	uintptr_t physical_address;
	size_t    align_pages;
	uint32_t  flags;
};

/* Parameters used to create one memory object. */
struct memory_create_params {
	size_t                    page_count;
	enum memory_type          memory_type;
	struct memory_constraints constraints;
};

/* Operations supported by a memory object capability. */
enum memory_op {
	MEMORY_OP_INFO = 0,
	MEMORY_OP_READ,
	MEMORY_OP_WRITE,
};

/* Operations supported by a mapping control capability. */
enum mapping_op {
	MAPPING_OP_INFO = 0,
	MAPPING_OP_PROTECT,
	MAPPING_OP_UNMAP,
};

/* Immutable logical metadata for a memory object. */
struct memory_info {
	size_t           page_count;
	enum memory_type memory_type;
};

/* Common header for memory object capability requests. */
struct memory_request_header {
	enum memory_op op;
};

/* Request to read immutable memory object metadata. */
struct memory_info_request {
	struct memory_request_header header;
};

/* Request to copy memory object contents into the caller's address space. */
struct memory_read_request {
	struct memory_request_header header;
	size_t                       offset;
	uintptr_t                    destination;
	size_t                       size;
};

/* Request to copy caller contents into a memory object. */
struct memory_write_request {
	struct memory_request_header header;
	uintptr_t                    source;
	size_t                       offset;
	size_t                       size;
};

/* Response reporting a completed memory object byte transfer. */
struct memory_transfer_response {
	size_t bytes_transferred;
};

/* Common header for mapping control capability requests. */
struct mapping_request_header {
	enum mapping_op op;
};

/* Request to read persistent mapping metadata. */
struct mapping_info_request {
	struct mapping_request_header header;
};

/* Request to change one mapping's protection bits. */
struct mapping_protect_request {
	struct mapping_request_header header;
	vmm_prot_t                    prot;
};

/* Request to explicitly destroy one mapping. */
struct mapping_unmap_request {
	struct mapping_request_header header;
};

/* Response containing persistent mapping metadata. */
struct mapping_info_response {
	struct vmm_info info;
};
