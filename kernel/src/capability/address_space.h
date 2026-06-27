#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/vmm.h>
#include <core/process.h>

/* Creates and returns an address space capability for process. Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_address_space_grant(struct process* process, cap_rights_t rights);
