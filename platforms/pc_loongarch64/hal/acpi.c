#include <hal/acpi.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts/controllers.h"
#include "iommu/iommu.h"

static size_t acpi_append(size_t (*provider)(struct hal_acpi_consumed_table*, size_t),
                          struct hal_acpi_consumed_table* tables, size_t capacity, size_t count) {
	size_t available = count < capacity ? capacity - count : 0u;
	size_t added     = provider(tables != NULL && available != 0u ? tables + count : NULL, available);

	return added > SIZE_MAX - count ? SIZE_MAX : count + added;
}

size_t hal_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	size_t count = 0u;

	count = acpi_append(loongarch64_interrupt_acpi_consumed_tables, tables, capacity, count);
	count = acpi_append(loongarch64_iommu_acpi_consumed_tables, tables, capacity, count);
	return count;
}
