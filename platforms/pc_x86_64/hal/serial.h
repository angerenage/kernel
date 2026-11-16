#pragma once

#include <stddef.h>

#include <hal/acpi.h>

/* List ACPI tables consumed by the x86_64 serial implementation. */
size_t x86_64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);
