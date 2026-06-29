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
bool cap_publish(channel_id_t endpoint_id, uint64_t object_id, process_id_t target, cap_rights_t rights, cap_id_t* out);

/* Delegate an existing capability to another process with a subset of rights. */
bool cap_delegate(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out);

/* Derive a new capability on a different object within the same endpoint. */
bool cap_derive(cap_id_t base, process_id_t target, uint64_t object_id, cap_rights_t rights, cap_id_t* out);

/* Revoke a subset of rights from a capability (or the whole capability when rights == 0). */
bool cap_revoke(cap_id_t cap, cap_rights_t rights);

/* Invoke a capability with a typed request and optional response. */
bool cap_call(cap_id_t cap, const void* request, size_t request_size, void* response, size_t response_capacity,
              size_t* result_value);
