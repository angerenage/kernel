#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Publish an i8042 controller from the FADT using derived I/O-port resources. */
syscall_status_t acpi_fadt_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t io_ports_cap, size_t* device_count);
