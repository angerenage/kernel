#pragma once

#include <base/acpi.h>
#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Return the stable number of userspace-visible tables matching signature. */
syscall_status_t acpi_table_count(cap_id_t provider_cap, const char signature[4], uint64_t* out_count);

/* Exclusively claim one table instance. */
syscall_status_t acpi_table_claim(cap_id_t provider_cap, const char signature[4], uint64_t index,
                                  cap_id_t* out_table_cap);

/* Read revision and headerless body size from a claimed table. */
syscall_status_t acpi_table_info(cap_id_t table_cap, struct acpi_table_info_response* out_info);

/* Copy an exact range from a claimed table's headerless body. */
syscall_status_t acpi_table_read(cap_id_t table_cap, uint64_t offset, void* buffer, size_t size);
