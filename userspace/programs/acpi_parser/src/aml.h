#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Load sanitized AML and publish every present or functioning Device object. */
syscall_status_t acpi_aml_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                cap_id_t io_ports_cap, size_t* device_count);
