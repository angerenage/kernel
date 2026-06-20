#pragma once

#include <base/cap.h>
#include <core/process.h>

/* Creates and returns the self capability for process. Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_self_grant(struct process* process);
