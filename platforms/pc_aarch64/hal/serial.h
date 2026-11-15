#pragma once

#include <stddef.h>

#include <hal/acpi.h>
#include <hal/device_tree.h>

/* Describe the ACPI console table owned by the serial HAL. */
size_t aarch64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);

/* Describe the Device Tree console node owned by the serial HAL. */
size_t aarch64_serial_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);
