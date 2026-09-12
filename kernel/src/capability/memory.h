#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/mapping.h>
#include <core/memory.h>
#include <core/process.h>

struct address_space;

/* Publish independent Memory authority for recipient. */
cap_id_t kernel_memory_grant(struct memory* memory, process_id_t recipient, cap_rights_t rights);

/* Resolve and retain the Memory referenced by an authorized capability. */
syscall_result_t kernel_memory_acquire(cap_id_t memory_cap, process_id_t caller, cap_rights_t required_rights,
                                       struct cap_object** out_object, struct memory** out_memory,
                                       cap_rights_t* out_rights);

/* Publish the unique capability resource for an active process Mapping. */
cap_id_t kernel_mapping_publish(struct process* target, process_id_t recipient, struct mapping* mapping,
                                cap_rights_t rights, memory_access_t maximum_access);

/* Publish the unique capability resource for an active DEVICE Mapping. */
cap_id_t kernel_device_mapping_publish(struct address_space* space, process_id_t recipient, struct mapping* mapping,
                                       cap_rights_t rights, memory_access_t maximum_access);

/* Roll back an unpublished Mapping capability and its projection. */
bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner);
