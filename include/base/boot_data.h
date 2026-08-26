#pragma once

#include <base/kernel_resource.h>
#include <stddef.h>
#include <stdint.h>

/* Operations supported by immutable boot-data capabilities such as RSDP and DTB. */
enum boot_data_op {
	/* Return the resource type and total byte size. */
	BOOT_DATA_OP_INFO = 0,
	/* Copy a bounded byte range into the capability response. */
	BOOT_DATA_OP_READ,
};

/* Common prefix for every immutable boot-data request. */
struct boot_data_request_header {
	enum boot_data_op op;
};

/* Request the boot-data resource type and size. */
struct boot_data_info_request {
	struct boot_data_request_header header;
};

/* Stable kernel-resource type and total readable byte size. */
struct boot_data_info_response {
	enum kernel_resource_type type;
	size_t                    size;
};

/* Request size bytes beginning at offset; the entire range must be in bounds. */
struct boot_data_read_request {
	struct boot_data_request_header header;
	uint64_t                        offset;
	size_t                          size;
};
