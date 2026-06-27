#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <core/capability.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Create a kernel-owned cap_object and a root capability granting rights on it to target. Returns CAP_ID_INVALID on
 * failure. */
cap_id_t cap_kernel_create(uint64_t object_id, cap_kernel_handler_t handler, process_id_t target, cap_rights_t rights);

/* Return whether a call supplied enough response storage for response_size bytes. */
bool cap_kernel_response_fits(const struct cap_request* request, size_t response_size);

/* Copy a typed response into a kernel capability call's response buffer. */
syscall_result_t cap_kernel_write_response(const struct cap_request* request, const void* response,
                                           size_t response_size);

/* Initialize kernel capability objects. Called after capability_init(). */
void kernel_capability_init(void);
