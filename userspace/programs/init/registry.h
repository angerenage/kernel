#pragma once

#include <runtime/init.h>
#include <stdbool.h>

/* Add or replace an advertisement owned by one process. */
enum init_registry_status registry_advertise(process_id_t owner, const struct init_service_selector* selector,
                                             uint32_t minor, cap_id_t capability, cap_rights_t client_rights);

/* Withdraw one live advertisement when the caller is its owner. */
enum init_registry_status registry_withdraw(process_id_t owner, const struct init_service_selector* selector);

/* Acquire and delegate one named compatible service to the caller. */
enum init_registry_status registry_acquire(process_id_t caller, const struct init_protocol_query* query,
                                           const char* service, struct init_service_handle* out_handle);

/* Fetch an offset/size page of compatible service metadata and the current total. */
enum init_registry_status registry_enumerate(const struct init_protocol_query* query, uint64_t offset, uint64_t size,
                                             struct init_service_info* entries, uint64_t* out_returned,
                                             uint64_t* out_total);

/* Fetch an offset/size page of immediate children below a namespace. */
enum init_registry_status registry_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                          struct init_browse_entry* entries, uint64_t* out_returned,
                                          uint64_t* out_total);

/* Register a protocol watch and send its initial counter value. */
enum init_registry_status registry_watch(process_id_t owner, const struct init_protocol_query* query,
                                         cap_id_t signal_capability, uint64_t* out_subscription_id,
                                         uint64_t* out_counter);

/* Remove a registry watch when the caller is its owner. */
enum init_registry_status registry_unwatch(process_id_t owner, uint64_t subscription_id);

/* Validate a normalized namespace path, optionally accepting the root path. */
bool registry_namespace_valid(const char* namespace_path, bool allow_root);

/* Validate one protocol, service, or namespace-component name. */
bool registry_name_valid(const char* name);

/* Validate all names carried by a protocol query. */
bool registry_query_valid(const struct init_protocol_query* query);

/* Validate all names carried by a service selector. */
bool registry_selector_valid(const struct init_service_selector* selector);
