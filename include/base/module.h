#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t module_id_t;

#define MODULE_ID_INVALID ((module_id_t)0u)
#define MODULE_NAME_CAPACITY 64u
#define MODULE_PATH_CAPACITY 256u

/* Operations supported by the modules-provider capability. */
enum module_provider_op {
	MODULE_PROVIDER_OP_RESOLVE   = 0,
	MODULE_PROVIDER_OP_ENUMERATE = 1,
};

struct module_provider_request_header {
	enum module_provider_op op;
};

/* Resolve a boot module through the provider. The request is followed by name_size bytes. */
struct module_provider_resolve_request {
	struct module_provider_request_header header;
	size_t                                name_size;
};

/* Boot-module metadata and capability returned when resolving by name. */
struct module_provider_resolve_response {
	module_id_t id;
	cap_id_t    cap;
	size_t      size;
	uint32_t    media_type;
	char        name[MODULE_NAME_CAPACITY];
	char        path[MODULE_PATH_CAPACITY];
};

/* Request one stable page of boot-module metadata in provider order. */
struct module_provider_enumerate_request {
	struct module_provider_request_header header;
	uint32_t                              reserved;
	uint64_t                              offset;
	uint64_t                              count;
};

/* Fixed-size boot-module descriptor returned by provider enumeration. */
struct module_provider_entry {
	module_id_t id;
	uint64_t    size;
	uint32_t    media_type;
	uint32_t    reserved;
	char        name[MODULE_NAME_CAPACITY];
	char        path[MODULE_PATH_CAPACITY];
};

/* Total provider size and the descriptors returned for the requested page. */
struct module_provider_enumerate_response {
	uint64_t                     total;
	uint64_t                     returned;
	struct module_provider_entry entries[];
};

/* Operation codes for boot-module capability requests. */
enum module_op {
	MODULE_OP_INFO = 0,
	MODULE_OP_MAP  = 1,
	MODULE_OP_READ = 2,
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

/* Read a bounded byte range from a boot module. Bytes are returned directly as the capability response. */
struct module_read_request {
	struct module_request_header header;
	uint64_t                     offset;
	size_t                       size;
};

/* Descriptive metadata returned by MODULE_OP_INFO. */
struct module_info_response {
	module_id_t id;
	size_t      size;
	uint32_t    media_type;
	char        name[MODULE_NAME_CAPACITY];
	char        path[MODULE_PATH_CAPACITY];
};

/* Mapping capability, initial mapping snapshot, and module-data offset returned by MODULE_OP_MAP. */
struct module_map_response {
	cap_id_t  mapping_cap;
	uintptr_t address;
	size_t    mapping_size;
	size_t    data_offset;
};
