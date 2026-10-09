#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Parse supported ACPI tables and commit every valid device through root_cap. */
syscall_status_t acpi_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, size_t* out_device_count);
