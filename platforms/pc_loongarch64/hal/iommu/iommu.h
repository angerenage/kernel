#pragma once

#include <hal/acpi.h>

/* List ACPI tables consumed by the LoongArch IOMMU implementation. */
size_t loongarch64_iommu_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);
