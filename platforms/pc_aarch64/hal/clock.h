#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hal/acpi.h>
#include <hal/device_tree.h>

/* List the ACPI tables consumed by the AArch64 clock HAL provider. */
size_t aarch64_clock_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);

/* List the Device Tree nodes consumed by the AArch64 clock HAL provider. */
size_t aarch64_clock_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);

/* Return whether an interrupt ID is the discovered virtual timer source. */
bool aarch64_clock_is_interrupt(uint32_t id);

/* Service one virtual timer interrupt. */
bool aarch64_clock_fire(void);
