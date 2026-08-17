#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Init service capability installed from process_startup_info by _start. */
extern cap_id_t init_cap_id;

#define INIT_SERVICE_OBJECT_ID 1u

enum {
	INIT_NAMESPACE_PATH_MAX = 255u,
	INIT_NAME_MAX           = 63u,
};

enum init_op {
	INIT_OP_GET_INFO = 0,
	INIT_OP_ADVERTISE,
	INIT_OP_WITHDRAW,
	INIT_OP_LOOKUP,
	INIT_OP_ENUMERATE,
	INIT_OP_BROWSE,
	INIT_OP_WATCH,
	INIT_OP_UNWATCH,
};

enum init_registry_status {
	INIT_REGISTRY_OK = 0,
	INIT_REGISTRY_INVALID_ARGUMENT,
	INIT_REGISTRY_NOT_FOUND,
	INIT_REGISTRY_CONFLICT,
	INIT_REGISTRY_DENIED,
	INIT_REGISTRY_NO_MEMORY,
	INIT_REGISTRY_DELIVERY_FAILED,
	INIT_REGISTRY_RESOURCE_FAILURE,
	INIT_REGISTRY_TRANSPORT_ERROR,
};

struct init_call_result {
	enum init_registry_status status;
	syscall_status_t          transport_status;
};

struct init_protocol_query {
	char     namespace_path[INIT_NAMESPACE_PATH_MAX + 1u];
	char     protocol[INIT_NAME_MAX + 1u];
	uint32_t major;
	uint32_t minor;
};

struct init_service_selector {
	char     namespace_path[INIT_NAMESPACE_PATH_MAX + 1u];
	char     protocol[INIT_NAME_MAX + 1u];
	uint32_t major;
	char     service[INIT_NAME_MAX + 1u];
};

struct init_service_entry {
	struct init_service_selector selector;
	uint32_t                     minor;
	cap_id_t                     capability;
	cap_rights_t                 rights;
};

enum init_browse_entry_kind {
	INIT_BROWSE_NAMESPACE = 0,
	INIT_BROWSE_PROTOCOL,
};

struct init_browse_entry {
	enum init_browse_entry_kind kind;
	char                        name[INIT_NAME_MAX + 1u];
	uint32_t                    major;
	uint32_t                    minor;
};

struct init_request_header {
	enum init_op op;
};

struct init_get_info_request {
	struct init_request_header header;
};

struct init_get_info_response {
	enum init_registry_status status;
	process_id_t              init_pid;
};

struct init_advertise_request {
	struct init_request_header   header;
	struct init_service_selector selector;
	uint32_t                     minor;
	cap_id_t                     capability;
	cap_rights_t                 client_rights;
};

struct init_registry_response {
	enum init_registry_status status;
};

struct init_withdraw_request {
	struct init_request_header   header;
	struct init_service_selector selector;
};

struct init_lookup_request {
	struct init_request_header header;
	struct init_protocol_query query;
	char                       service[INIT_NAME_MAX + 1u];
};

struct init_lookup_response {
	enum init_registry_status status;
	struct init_service_entry entry;
};

struct init_enumerate_request {
	struct init_request_header header;
	struct init_protocol_query query;
	uint64_t                   offset;
	uint64_t                   size;
};

struct init_enumerate_response {
	enum init_registry_status status;
	uint64_t                  total;
	uint64_t                  returned;
	struct init_service_entry entries[];
};

struct init_browse_request {
	struct init_request_header header;
	char                       namespace_path[INIT_NAMESPACE_PATH_MAX + 1u];
	uint64_t                   offset;
	uint64_t                   size;
};

struct init_browse_response {
	enum init_registry_status status;
	uint64_t                  total;
	uint64_t                  returned;
	struct init_browse_entry  entries[];
};

struct init_watch_request {
	struct init_request_header header;
	struct init_protocol_query query;
	cap_id_t                   signal_capability;
};

struct init_watch_response {
	enum init_registry_status status;
	uint64_t                  subscription_id;
	uint64_t                  counter;
};

struct init_unwatch_request {
	struct init_request_header header;
	uint64_t                   subscription_id;
};

/* Advertise one exact service revision and automatically grant init the service capability. */
struct init_call_result init_advertise(const struct init_service_selector* selector, uint32_t minor,
                                       cap_id_t service_capability, cap_rights_t client_rights);

/* Remove the calling process's advertisement for one service identity. */
struct init_call_result init_withdraw(const struct init_service_selector* selector);

/* Find one named service compatible with the requested protocol revision. */
struct init_call_result init_lookup(const struct init_protocol_query* query, const char* service,
                                    struct init_service_entry* out_entry);

/* Fetch one offset/size page of compatible services and report the current total. */
struct init_call_result init_enumerate(const struct init_protocol_query* query, uint64_t offset, uint64_t size,
                                       struct init_service_entry* entries, uint64_t* out_returned, uint64_t* out_total);

/* Fetch one offset/size page of immediate children below a namespace. */
struct init_call_result init_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                    struct init_browse_entry* entries, uint64_t* out_returned, uint64_t* out_total);

/* Watch a protocol query and automatically grant init permission to signal changes. */
struct init_call_result init_watch(const struct init_protocol_query* query, cap_id_t signal_capability,
                                   uint64_t* out_subscription_id, uint64_t* out_counter);

/* Remove one registry watch owned by the calling process. */
struct init_call_result init_unwatch(uint64_t subscription_id);
