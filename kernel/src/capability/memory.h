#pragma once

#include <base/address_space.h>
#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/process.h>
#include <stdbool.h>

/* Create a memory object capability for the current process. */
cap_id_t kernel_memory_create(cap_rights_t rights, size_t page_count);

/* Map a memory capability into a target address space. */
syscall_result_t kernel_memory_map(cap_id_t memory_cap, process_id_t caller, struct process* target,
                                   const struct memory_map_params* params, struct address_space_map_result* out_result);

/* Grant control over an existing mapping without taking mapping ownership. */
cap_id_t kernel_mapping_grant(struct process* target, process_id_t recipient, vmm_id_t mapping_id, cap_rights_t rights);

/* Roll back an undelivered mapping result and its control capability. */
bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner);

/* Return the private mapping control-state size for structural tests. */
size_t kernel_mapping_state_size(void);
