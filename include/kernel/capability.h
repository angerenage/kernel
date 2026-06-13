#pragma once

#include <base/cap.h>
#include <base/id.h>
#include <base/process.h>
#include <core/capability.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern id_allocator_t cap_kernel_object_id_allocator;

/* Create a kernel-owned cap_object and a root capability granting rights on it to target. Returns CAP_ID_INVALID on
 * failure. */
cap_id_t cap_kernel_create(uint64_t object_id, cap_kernel_handler_t handler, process_id_t target, cap_rights_t rights);

/* Initialize kernel capability objects. Called after capability_init(). */
void kernel_capability_init(void);
