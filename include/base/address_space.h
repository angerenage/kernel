#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Operation codes for address space capability requests. */
enum address_space_op {
	ADDRESS_SPACE_OP_QUERY = 0,
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
