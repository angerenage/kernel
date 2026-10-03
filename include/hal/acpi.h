#pragma once

#include <stddef.h>

/* ACPI table signature consumed by the kernel. */
struct hal_acpi_consumed_table {
	char signature[4] __attribute__((nonstring));
};

/* List the ACPI tables consumed by all platform HAL providers. */
size_t hal_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);
