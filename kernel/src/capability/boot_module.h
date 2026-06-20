#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stddef.h>
#include <stdint.h>

/* Initializes the kernel boot module capability system. */
void kernel_capability_boot_module_init(void);

/* Creates and returns a capability for the boot module at the given index to a process. Returns CAP_ID_INVALID on
 * failure. */
cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t target);
