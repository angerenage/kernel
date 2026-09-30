#pragma once

#include <stdint.h>

/* Stable identifiers for device descriptor structures exposed by the kernel. */
enum kernel_device_type {
	KERNEL_DEVICE_TYPE_INVALID = 0u,
	KERNEL_DEVICE_TYPE_PCI,
};

/* Operations accepted by the kernel Devices resource. */
enum kernel_devices_op {
	KERNEL_DEVICES_OP_COUNT = 0,
	KERNEL_DEVICES_OP_LIST,
};

/* Common prefix for every kernel Devices request. */
struct kernel_devices_request_header {
	enum kernel_devices_op op;
};

/* Request the number of descriptors registered for one device type. */
struct kernel_devices_count_request {
	struct kernel_devices_request_header header;
	enum kernel_device_type              type;
};

/* Number of descriptors registered for a device type. */
struct kernel_devices_count_response {
	uint64_t count;
};

/* Request at most length descriptors beginning at the type-local offset. */
struct kernel_devices_list_request {
	struct kernel_devices_request_header header;
	enum kernel_device_type              type;
	uint64_t                             offset;
	uint64_t                             length;
	uint64_t                             element_size;
};

/* Variable-length response followed by returned type-specific C structures. */
struct kernel_devices_list_response {
	uint64_t returned;
	uint8_t  entries[];
};
