#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Create the Device Tree provider when a validated tree is available. */
bool kernel_capability_device_tree_init(void);

/* Return whether the Device Tree provider can currently be granted. */
bool kernel_capability_device_tree_available(void);

/* Grant authority to inspect the userspace-visible Device Tree. */
cap_id_t kernel_capability_device_tree_grant(process_id_t recipient);
