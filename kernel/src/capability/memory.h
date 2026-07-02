#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <core/capability.h>
#include <core/process.h>
#include <stdbool.h>

/* Creates a new allocation capability with the requested pages and grants it to the calling process.
 * Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_allocate_memory(cap_rights_t rights, size_t page_count, vmm_prot_t prot, enum vmm_kind kind);

/* Map an allocation capability into target's address space and grant the resulting mapping to caller. */
syscall_result_t kernel_map_allocation(cap_id_t allocation_cap_id, process_id_t caller, struct process* target,
                                       uintptr_t address, cap_id_t* out_mapping_cap);

/* Grant control over an existing VMM region. The mapping capability owns the region, not its backing resource. */
cap_id_t kernel_mapping_grant(struct process* target, process_id_t recipient, vmm_id_t region_id, enum vmm_kind kind,
                              cap_rights_t rights);

/* Roll back a mapping capability that has not yet been published to its recipient. */
bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner);
