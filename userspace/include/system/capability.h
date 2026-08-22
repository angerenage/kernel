#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Raw capability call returning the kernel's syscall_result_t envelope. */
syscall_result_t cap_call_syscall(cap_id_t cap, const void* request, size_t request_size, void* response,
                                  size_t response_capacity);

/* Publish a userspace capability object under an endpoint channel. The caller must own the endpoint. */
syscall_status_t cap_publish(channel_id_t endpoint_id, uint64_t object_id, process_id_t target, cap_rights_t rights,
                             cap_id_t* out);

/* Delegate an existing capability to another process with a subset of rights. */
syscall_status_t cap_delegate(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out);

/* Delegate as a sibling of source. The source must hold CAP_DELEGATE_PEER. */
syscall_status_t cap_delegate_peer(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out);

/* Derive a new capability on a different object within the same endpoint. */
syscall_status_t cap_derive(cap_id_t base, process_id_t target, uint64_t object_id, cap_rights_t rights, cap_id_t* out);

/* Revoke a subset of rights from a capability (or the whole capability when rights == 0). */
syscall_status_t cap_revoke(cap_id_t cap, cap_rights_t rights);

/* Drop a capability owned by the caller without revoking capabilities delegated from it. */
syscall_status_t cap_drop(cap_id_t cap);

/* Unpublish a userspace object owned through an endpoint channel. */
syscall_status_t cap_unpublish(channel_id_t endpoint_id, uint64_t object_id);

/* Report whether a capability owned by the caller still exists. */
syscall_status_t cap_valid(cap_id_t cap, bool* out_valid);

/* Invoke a capability with a typed request and optional response. */
syscall_status_t cap_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                          size_t response_capacity, size_t* result_value);
