#pragma once

#include <base/cap.h>
#include <base/syscall.h>

#include "../device.h"

/* Stage an i8042 fallback only when AML did not report PNP0303 and the FADT reports a controller. */
syscall_status_t acpi_fadt_parse(cap_id_t provider_cap, struct acpi_device_set* devices, cap_id_t interrupts_cap,
                                 cap_id_t io_ports_cap);
