#pragma once

#include <base/cap.h>
#include <base/syscall.h>

#include "../device.h"

/* Parse every visible TPM2 table and stage its supported TPM observations. */
syscall_status_t acpi_tpm2_parse(cap_id_t provider_cap, struct acpi_device_set* devices, cap_id_t memory_allocator_cap);
