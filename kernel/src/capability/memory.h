#pragma once
#include <base/address_space.h>
#include <base/cap.h>
#include <base/memory.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/process.h>
#include <stdbool.h>

/* Create a memory capability for the current process. */
cap_id_t kernel_memory_create(cap_rights_t rights, const struct memory_create_params* params);

/* Return whether one legacy memory creation request is valid. */
bool kernel_memory_create_params_valid(const struct memory_create_params* params);

/* Map a memory capability into a target address space. */
syscall_result_t kernel_memory_map(cap_id_t memory_cap, process_id_t caller, struct process* target,
                                   const struct memory_map_params* params, struct address_space_map_result* out_result);

/* Grant legacy control over an existing Mapping projection. */
cap_id_t kernel_mapping_grant(struct process* target, process_id_t recipient, struct mapping* mapping,
                              size_t legacy_memory_page_offset, cap_rights_t rights);

/* Roll back an undelivered mapping result and its control capability. */
bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner);
