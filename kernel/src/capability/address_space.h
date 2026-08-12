#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <core/process.h>

/* Grant recipient a capability for process' address space. */
cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights,
                                    bool* out_created);
