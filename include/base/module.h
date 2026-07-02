#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t module_id_t;

#define MODULE_ID_INVALID ((module_id_t)0u)
#define MODULE_NAME_CAPACITY 64u
#define MODULE_PATH_CAPACITY 256u

/* Boot-module metadata and capability returned when resolving by name. */
struct module_query_response {
	module_id_t id;
	cap_id_t    cap;
	size_t      size;
	uint32_t    media_type;
	char        name[MODULE_NAME_CAPACITY];
	char        path[MODULE_PATH_CAPACITY];
};

/* Operation codes for boot-module capability requests. */
enum module_op {
	MODULE_OP_INFO = 0,
	MODULE_OP_MAP  = 1,
};

/* Common header for all boot-module capability requests. */
struct module_request_header {
	enum module_op op;
};

/* Request the descriptive metadata associated with a boot module. */
struct module_info_request {
	struct module_request_header header;
};

/* Map a boot module read-only into the caller's address space. */
struct module_map_request {
	struct module_request_header header;
};

/* Descriptive metadata returned by MODULE_OP_INFO. */
struct module_info_response {
	module_id_t id;
	size_t      size;
	uint32_t    media_type;
	char        name[MODULE_NAME_CAPACITY];
	char        path[MODULE_PATH_CAPACITY];
};

/* Address returned by MODULE_OP_MAP. */
struct module_map_response {
	uintptr_t mapped_base;
};
