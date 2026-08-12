#pragma once

#include <base/cap.h>
#include <core/process.h>

/* Grant recipient a capability for target with the specified rights. */
cap_id_t kernel_process_grant(struct process* target, process_id_t recipient, cap_rights_t rights, bool* out_created);

/* Creates and returns the self capability for process. Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_self_grant(struct process* process, bool* out_created);
