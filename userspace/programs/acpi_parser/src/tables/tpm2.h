#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Parse every visible TPM2 table and append its supported TPM devices. */
syscall_status_t acpi_tpm2_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                 size_t* device_count);
