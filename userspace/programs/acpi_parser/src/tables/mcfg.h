#pragma once

#include <base/cap.h>
#include <base/syscall.h>

#include "../device.h"

/* Attach MCFG fields to their AML device, or create an MCFG fallback when no device matches. */
syscall_status_t acpi_mcfg_parse(cap_id_t provider_cap, struct acpi_device_set* devices, cap_id_t memory_allocator_cap);
