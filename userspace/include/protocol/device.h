#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Protocol limits for diagnostic names, compatible IDs, and lower-snake-case identifiers. */
#define DEVICE_NAME_MAX 255u
#define DEVICE_COMPATIBLE_ID_MAX 255u
#define DEVICE_IDENTIFIER_MAX 63u

/* Per-builder retained-state and collection limits. */
#define DEVICE_MAX_STATE_SIZE (1024u * 1024u)
#define DEVICE_MAX_PROPERTIES 1024u
#define DEVICE_MAX_COMPATIBLE_IDS 64u
#define DEVICE_MAX_RESOURCES 64u

/* Rights held by the manager on its private root-construction capability. */
#define DEVICE_ROOT_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_MANAGE | CAP_DELEGATE))
/* Exact rights granted on a non-delegable construction capability. */
#define DEVICE_BUILDER_CAP_RIGHTS ((cap_rights_t)CAP_CALL)
/* Minimum rights used for read-only access to a committed device. */
#define DEVICE_CAP_READ_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))
/* Maximum rights retained by the manager for a future bound driver. */
#define DEVICE_CAP_DRIVER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Property value kinds supported by immutable device metadata. */
enum device_property_type {
	DEVICE_PROPERTY_INVALID = 0u,
	DEVICE_PROPERTY_BOOLEAN,
	DEVICE_PROPERTY_UNSIGNED,
	DEVICE_PROPERTY_SIGNED,
	DEVICE_PROPERTY_UTF8_STRING,
	DEVICE_PROPERTY_BYTES,
	DEVICE_PROPERTY_BOOLEAN_ARRAY,
	DEVICE_PROPERTY_UNSIGNED_ARRAY,
	DEVICE_PROPERTY_SIGNED_ARRAY,
	DEVICE_PROPERTY_UTF8_STRING_ARRAY,
};

/* Immutable collection sizes and optional-name length for one device. */
struct device_info {
	uint64_t name_size;
	uint64_t compatible_count;
	uint64_t property_count;
	uint64_t resource_count;
};

/* Type, logical element count, and encoded byte size for one property. */
struct device_property_info {
	enum device_property_type type;
	uint32_t                  reserved;
	uint64_t                  element_count;
	uint64_t                  value_size;
};

/* Name length and maximum acquirable rights for one named resource. */
struct device_resource_info {
	uint64_t     name_size;
	cap_rights_t rights;
};

/* Operations accepted by a committed device capability. */
enum device_object_op {
	DEVICE_OBJECT_OP_INFO = 0u,
	DEVICE_OBJECT_OP_READ_NAME,
	DEVICE_OBJECT_OP_COMPATIBLE_INFO,
	DEVICE_OBJECT_OP_READ_COMPATIBLE,
	DEVICE_OBJECT_OP_PROPERTY_INFO,
	DEVICE_OBJECT_OP_READ_PROPERTY,
	DEVICE_OBJECT_OP_RESOURCE_INFO,
	DEVICE_OBJECT_OP_READ_RESOURCE_NAME,
	DEVICE_OBJECT_OP_ACQUIRE_RESOURCE,
	DEVICE_OBJECT_OP_BEGIN_CHILD,
};

/* Common operation header for committed-device requests. */
struct device_object_request_header {
	enum device_object_op op;
};

/* Request containing only a committed-device operation. */
struct device_object_simple_request {
	struct device_object_request_header header;
};

/* Request selecting one compatible ID or resource by provider-order index. */
struct device_object_index_request {
	struct device_object_request_header header;
	uint32_t                            reserved;
	uint64_t                            index;
};

/* Request reading a byte range from one indexed compatible ID or resource name. */
struct device_object_read_index_request {
	struct device_object_request_header header;
	uint32_t                            reserved;
	uint64_t                            index;
	uint64_t                            offset;
	uint64_t                            size;
};

/* A lower-snake-case property name follows this header. */
struct device_object_property_info_request {
	struct device_object_request_header header;
	uint32_t                            name_size;
	uint8_t                             name[];
};

/* A lower-snake-case property name follows this header. */
struct device_object_property_read_request {
	struct device_object_request_header header;
	uint32_t                            name_size;
	uint64_t                            offset;
	uint64_t                            size;
	uint8_t                             name[];
};

/* A lower-snake-case resource name follows this header. */
struct device_object_resource_acquire_request {
	struct device_object_request_header header;
	uint32_t                            name_size;
	cap_rights_t                        rights;
	uint8_t                             name[];
};

/* Capability delegated to the caller after successful named-resource acquisition. */
struct device_object_resource_acquire_response {
	cap_id_t capability;
};

/* Operations accepted by the manager's private root-construction capability. */
enum device_root_op {
	DEVICE_ROOT_OP_BEGIN = 0u,
};

/* Begin an independent root-device transaction. */
struct device_root_begin_request {
	enum device_root_op op;
};

/* Non-delegable builder and manager PID returned when construction begins. */
struct device_builder_begin_response {
	cap_id_t     builder_cap;
	process_id_t manager_pid;
};

/* Operations accepted by a temporary device-builder capability. */
enum device_builder_op {
	DEVICE_BUILDER_OP_SET_NAME = 0u,
	DEVICE_BUILDER_OP_ADD_COMPATIBLE,
	DEVICE_BUILDER_OP_BEGIN_PROPERTY,
	DEVICE_BUILDER_OP_APPEND_PROPERTY,
	DEVICE_BUILDER_OP_FINISH_PROPERTY,
	DEVICE_BUILDER_OP_ADD_RESOURCE,
	DEVICE_BUILDER_OP_COMMIT,
	DEVICE_BUILDER_OP_ABORT,
};

/* Common operation header for builder requests. */
struct device_builder_request_header {
	enum device_builder_op op;
};

/* The UTF-8 name follows this header. */
struct device_builder_set_name_request {
	struct device_builder_request_header header;
	uint32_t                             name_size;
	uint8_t                              name[];
};

/* The UTF-8 compatible ID follows this header. */
struct device_builder_add_compatible_request {
	struct device_builder_request_header header;
	uint32_t                             id_size;
	uint8_t                              id[];
};

/* The lower-snake-case property name follows this header. */
struct device_builder_begin_property_request {
	struct device_builder_request_header header;
	enum device_property_type            type;
	uint32_t                             name_size;
	uint32_t                             reserved;
	uint64_t                             element_count;
	uint64_t                             value_size;
	uint8_t                              name[];
};

/* One contiguous property-value chunk follows this header. */
struct device_builder_append_property_request {
	struct device_builder_request_header header;
	uint32_t                             reserved;
	uint64_t                             offset;
	uint8_t                              bytes[];
};

/* The lower-snake-case resource name follows this header. */
struct device_builder_add_resource_request {
	struct device_builder_request_header header;
	uint32_t                             name_size;
	cap_id_t                             capability;
	cap_rights_t                         rights;
	uint8_t                              name[];
};

/* Builder request containing no operation-specific payload. */
struct device_builder_simple_request {
	struct device_builder_request_header header;
};
