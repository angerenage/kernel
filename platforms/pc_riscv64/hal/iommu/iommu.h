#pragma once

#include <hal/acpi.h>
#include <hal/device_tree.h>

/* List ACPI tables consumed by the RISC-V IOMMU implementation. */
size_t riscv64_iommu_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);

/* List Device Tree nodes consumed by the RISC-V IOMMU implementation. */
size_t riscv64_iommu_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);
