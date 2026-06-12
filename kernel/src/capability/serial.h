#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stddef.h>
#include <stdint.h>

/* Creates the serial object. */
void kernel_capability_serial_init(void);

/* Grants a capability for the serial object to a process. */
cap_id_t kernel_capability_serial_grant(process_id_t target);
