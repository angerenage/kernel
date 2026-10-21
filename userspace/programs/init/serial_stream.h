#pragma once

#include <base/cap.h>
#include <base/syscall.h>

/* Expose init's raw serial capability as a write-only userspace Stream. */
syscall_status_t serial_stream_create(cap_id_t serial_cap, cap_id_t* out_stream_cap);

/* Resume forwarding writes to the serial port. */
void serial_stream_enable(void);

/* Silently accept future writes without forwarding them to the serial port. */
void serial_stream_disable(void);
