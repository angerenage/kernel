#pragma once

#include <runtime/registry.h>
#include <stdbool.h>

/* Add or replace an advertisement owned by one process. */
enum registry_status registry_state_advertise(process_id_t owner, const struct registry_service_selector* selector,
                                              uint32_t minor, cap_id_t capability, cap_rights_t client_rights);

/* Withdraw one live advertisement when the caller is its owner. */
enum registry_status registry_state_withdraw(process_id_t owner, const struct registry_service_selector* selector);

/* Acquire and delegate one named compatible service to the caller. */
enum registry_status registry_state_acquire(process_id_t caller, const struct registry_protocol_query* query,
                                            const char* service, struct registry_service_handle* out_handle);

/* Grant an advertised capability directly to a launched process with selected rights. */
enum registry_status registry_state_grant(process_id_t target, const struct registry_protocol_query* query,
                                          const char* service, cap_rights_t rights, cap_id_t* out_capability);

/* Report whether one exact service is currently advertised. */
bool registry_state_contains(const struct registry_service_selector* selector);

/* Fetch an offset/size page of compatible service metadata and the current total. */
enum registry_status registry_state_enumerate(const struct registry_protocol_query* query, uint64_t offset,
                                              uint64_t size, struct registry_service_info* entries,
                                              uint64_t* out_returned, uint64_t* out_total);

/* Fetch an offset/size page of immediate children below a namespace. */
enum registry_status registry_state_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                           struct registry_browse_entry* entries, uint64_t* out_returned,
                                           uint64_t* out_total);

/* Register a protocol watch and send its initial counter value. */
enum registry_status registry_state_watch(process_id_t owner, const struct registry_protocol_query* query,
                                          cap_id_t signal_capability, uint64_t* out_subscription_id,
                                          uint64_t* out_counter);

/* Remove a registry watch when the caller is its owner. */
enum registry_status registry_state_unwatch(process_id_t owner, uint64_t subscription_id);

/* Validate a normalized namespace path, optionally accepting the root path. */
bool registry_state_namespace_valid(const char* namespace_path, bool allow_root);

/* Validate one protocol, service, or namespace-component name. */
bool registry_state_name_valid(const char* name);

/* Validate all names carried by a protocol query. */
bool registry_state_query_valid(const struct registry_protocol_query* query);

/* Validate all names carried by a service selector. */
bool registry_state_selector_valid(const struct registry_service_selector* selector);
