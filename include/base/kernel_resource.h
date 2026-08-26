#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

enum kernel_resource_type {
	KERNEL_RESOURCE_TYPE_INVALID = 0u,
	KERNEL_RESOURCE_TYPE_MODULES,
	KERNEL_RESOURCE_TYPE_SERIAL,
	KERNEL_RESOURCE_TYPE_LOADER,
	KERNEL_RESOURCE_TYPE_FRAMEBUFFER,
};

enum kernel_resources_op {
	KERNEL_RESOURCES_OP_LIST = 0,
	KERNEL_RESOURCES_OP_ACQUIRE,
};

struct kernel_resources_request_header {
	enum kernel_resources_op op;
};

/* Enumerate at most capacity resource identifiers starting at offset. */
struct kernel_resources_list_request {
	struct kernel_resources_request_header header;
	uint64_t                               offset;
	uint64_t                               capacity;
};

/* Variable-length list response followed by returned kernel_resource_type values. */
struct kernel_resources_list_response {
	uint64_t                  total;
	uint64_t                  returned;
	enum kernel_resource_type ids[];
};

struct kernel_resource_acquire_request {
	struct kernel_resources_request_header header;
	enum kernel_resource_type              id;
};

struct kernel_resource_acquire_response {
	cap_id_t cap;
};
