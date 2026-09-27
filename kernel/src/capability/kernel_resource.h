#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>

/* Create the singleton kernel-resources capability object. */
bool kernel_capability_resources_init(void);

/* Grant the root kernel-resources capability to one process. */
cap_id_t kernel_capability_resources_grant(process_id_t recipient);
