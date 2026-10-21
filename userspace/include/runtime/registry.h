#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Registry service capability installed from process_startup_info by _start. */
extern cap_id_t registry_cap_id;

#define REGISTRY_SERVICE_OBJECT_ID 1u

enum {
	REGISTRY_NAMESPACE_PATH_MAX = 255u,
	REGISTRY_NAME_MAX           = 63u,
};

enum registry_op {
	REGISTRY_OP_GET_INFO = 0,
	REGISTRY_OP_ADVERTISE,
	REGISTRY_OP_WITHDRAW,
	REGISTRY_OP_ACQUIRE,
	REGISTRY_OP_GRANT,
	REGISTRY_OP_ENUMERATE,
	REGISTRY_OP_BROWSE,
	REGISTRY_OP_WATCH,
	REGISTRY_OP_UNWATCH,
};

enum registry_status {
	REGISTRY_STATUS_OK = 0,
	REGISTRY_STATUS_INVALID_ARGUMENT,
	REGISTRY_STATUS_NOT_FOUND,
	REGISTRY_STATUS_CONFLICT,
	REGISTRY_STATUS_DENIED,
	REGISTRY_STATUS_NO_MEMORY,
	REGISTRY_STATUS_DELIVERY_FAILED,
	REGISTRY_STATUS_RESOURCE_FAILURE,
	REGISTRY_STATUS_TRANSPORT_ERROR,
};

struct registry_call_result {
	enum registry_status status;
	syscall_status_t     transport_status;
};

struct registry_protocol_query {
	char     namespace_path[REGISTRY_NAMESPACE_PATH_MAX + 1u];
	char     protocol[REGISTRY_NAME_MAX + 1u];
	uint32_t major;
	uint32_t minor;
};

struct registry_service_selector {
	char     namespace_path[REGISTRY_NAMESPACE_PATH_MAX + 1u];
	char     protocol[REGISTRY_NAME_MAX + 1u];
	uint32_t major;
	char     service[REGISTRY_NAME_MAX + 1u];
};

struct registry_service_info {
	struct registry_service_selector selector;
	uint32_t                         minor;
	cap_rights_t                     rights;
};

struct registry_service_handle {
	struct registry_service_info info;
	process_id_t                 owner;
	cap_id_t                     capability;
};

enum registry_browse_entry_kind {
	REGISTRY_BROWSE_NAMESPACE = 0,
	REGISTRY_BROWSE_PROTOCOL,
};

struct registry_browse_entry {
	enum registry_browse_entry_kind kind;
	char                            name[REGISTRY_NAME_MAX + 1u];
	uint32_t                        major;
	uint32_t                        minor;
};

struct registry_request_header {
	enum registry_op op;
};

struct registry_get_info_request {
	struct registry_request_header header;
};

struct registry_get_info_response {
	enum registry_status status;
	process_id_t         registry_pid;
};

struct registry_advertise_request {
	struct registry_request_header   header;
	struct registry_service_selector selector;
	uint32_t                         minor;
	cap_id_t                         capability;
	cap_rights_t                     client_rights;
};

struct registry_response {
	enum registry_status status;
};

struct registry_withdraw_request {
	struct registry_request_header   header;
	struct registry_service_selector selector;
};

struct registry_acquire_request {
	struct registry_request_header header;
	struct registry_protocol_query query;
	char                           service[REGISTRY_NAME_MAX + 1u];
};

struct registry_acquire_response {
	enum registry_status           status;
	struct registry_service_handle handle;
};

struct registry_grant_request {
	struct registry_request_header header;
	struct registry_protocol_query query;
	process_id_t                   target;
	cap_rights_t                   rights;
	char                           service[REGISTRY_NAME_MAX + 1u];
};

struct registry_grant_response {
	enum registry_status status;
	cap_id_t             capability;
};

struct registry_enumerate_request {
	struct registry_request_header header;
	struct registry_protocol_query query;
	uint64_t                       offset;
	uint64_t                       size;
};

struct registry_enumerate_response {
	enum registry_status         status;
	uint64_t                     total;
	uint64_t                     returned;
	struct registry_service_info entries[];
};

struct registry_browse_request {
	struct registry_request_header header;
	char                           namespace_path[REGISTRY_NAMESPACE_PATH_MAX + 1u];
	uint64_t                       offset;
	uint64_t                       size;
};

struct registry_browse_response {
	enum registry_status         status;
	uint64_t                     total;
	uint64_t                     returned;
	struct registry_browse_entry entries[];
};

struct registry_watch_request {
	struct registry_request_header header;
	struct registry_protocol_query query;
	cap_id_t                       signal_capability;
};

struct registry_watch_response {
	enum registry_status status;
	uint64_t             subscription_id;
	uint64_t             counter;
};

struct registry_unwatch_request {
	struct registry_request_header header;
	uint64_t                       subscription_id;
};

/* Advertise one exact service revision and automatically grant the registry the service capability. */
struct registry_call_result registry_advertise(const struct registry_service_selector* selector, uint32_t minor,
                                               cap_id_t service_capability, cap_rights_t client_rights);

/* Advertise a service while retaining broader capability rights inside the registry. */
struct registry_call_result registry_advertise_with_rights(const struct registry_service_selector* selector,
                                                           uint32_t minor, cap_id_t service_capability,
                                                           cap_rights_t registry_rights, cap_rights_t client_rights);

/* Remove the calling process's advertisement for one service identity. */
struct registry_call_result registry_withdraw(const struct registry_service_selector* selector);

/* Acquire one named service compatible with the requested protocol revision. */
struct registry_call_result registry_acquire(const struct registry_protocol_query* query, const char* service,
                                             struct registry_service_handle* out_handle);

/* Grant one named service directly to a process. This operation is restricted to init. */
struct registry_call_result registry_grant(process_id_t target, const struct registry_protocol_query* query,
                                           const char* service, cap_rights_t rights, cap_id_t* out_capability);

/* Fetch one offset/size page of compatible service metadata. */
struct registry_call_result registry_enumerate(const struct registry_protocol_query* query, uint64_t offset,
                                               uint64_t size, struct registry_service_info* entries,
                                               uint64_t* out_returned, uint64_t* out_total);

/* Fetch one offset/size page of immediate children below a namespace. */
struct registry_call_result registry_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                            struct registry_browse_entry* entries, uint64_t* out_returned,
                                            uint64_t* out_total);

/* Watch a protocol query and automatically grant the registry permission to signal changes. */
struct registry_call_result registry_watch(const struct registry_protocol_query* query, cap_id_t signal_capability,
                                           uint64_t* out_subscription_id, uint64_t* out_counter);

/* Remove one registry watch owned by the calling process. */
struct registry_call_result registry_unwatch(uint64_t subscription_id);
