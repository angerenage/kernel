#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Parse every visible MCFG table and append its ECAM configuration-window devices. */
syscall_status_t acpi_mcfg_parse(cap_id_t provider_cap, cap_id_t root_cap, size_t* device_count);
