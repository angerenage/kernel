#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates the loader object. */
void kernel_capability_loader_init(void);

bool kernel_capability_loader_available(void);

/* Grants a capability for the loader object to a process. */
cap_id_t kernel_capability_loader_grant(process_id_t recipient);
