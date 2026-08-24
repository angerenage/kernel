#pragma once

#include <base/cap.h>
#include <base/vmm.h>
#include <stddef.h>
#include <stdint.h>

/* Operations supported by an address space capability. */
enum address_space_op {
	ADDRESS_SPACE_OP_MAP = 0,
};

/* Parameters describing one memory object mapping. */
struct memory_map_params {
	size_t           memory_page_offset;
	size_t           page_count;
	uintptr_t        address;
	size_t           align_pages;
	size_t           guard_pages;
	vmm_prot_t       prot;
	enum memory_type memory_type;
};

/* Result of creating one mapping in an address space. */
struct address_space_map_result {
	cap_id_t        mapping_cap;
	struct vmm_info mapping;
};

/* Common header for address space capability requests. */
struct address_space_request_header {
	enum address_space_op op;
};

/* Request to map a memory object range into an address space. */
struct address_space_map_request {
	struct address_space_request_header header;
	cap_id_t                            memory_cap;
	struct memory_map_params            params;
};
