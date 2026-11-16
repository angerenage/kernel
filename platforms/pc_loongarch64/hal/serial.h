#pragma once

#include <stddef.h>

#include <hal/acpi.h>
#include <hal/device_tree.h>

/* List ACPI tables consumed by the LoongArch serial implementation. */
size_t loongarch64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);

/* List device tree nodes consumed by the LoongArch serial implementation. */
size_t loongarch64_serial_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);
