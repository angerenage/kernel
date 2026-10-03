#pragma once

#include <hal/acpi.h>

/* List ACPI tables consumed by the x86-64 IOMMU implementation. */
size_t x86_64_iommu_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);
