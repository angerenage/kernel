#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <core/capability.h>
#include <core/process.h>

/* Creates a new allocation capability with the requested pages and grants it to the calling process.
 * Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_allocate_memory(cap_rights_t rights, size_t page_count, vmm_prot_t prot, enum vmm_kind kind);

/* Map an allocation capability into target's address space and grant the resulting mapping to caller. */
syscall_result_t kernel_map_allocation(cap_id_t allocation_cap_id, process_id_t caller, struct process* target,
                                       uintptr_t address, cap_id_t* out_mapping_cap);
