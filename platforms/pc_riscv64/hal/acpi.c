#include <hal/acpi.h>
#include <stddef.h>

#include "iommu/iommu.h"

size_t hal_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	return riscv64_iommu_acpi_consumed_tables(tables, capacity);
}
