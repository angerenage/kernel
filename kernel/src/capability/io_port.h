#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>

/* Create the x86_64 root I/O-port range capability object. */
bool kernel_capability_io_ports_init(void);

/* Return whether the x86_64 I/O-port resource is available. */
bool kernel_capability_io_ports_available(void);

/* Grant the complete I/O-port range to one process. */
cap_id_t kernel_capability_io_ports_grant(process_id_t recipient);
