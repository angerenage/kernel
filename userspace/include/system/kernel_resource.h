#pragma once

#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Enumerate kernel resource IDs beginning at offset. */
syscall_status_t kernel_resources_list(cap_id_t kernel_resources_cap, uint64_t offset, enum kernel_resource_type* ids,
                                       size_t capacity, uint64_t* out_total, size_t* out_returned);

/* Acquire the provider or factory capability associated with one kernel resource ID. */
syscall_status_t kernel_resource_acquire(cap_id_t kernel_resources_cap, enum kernel_resource_type id,
                                         cap_id_t* out_cap);
