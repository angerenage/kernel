#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <hal/acpi.h>

#include "interrupts/frame.h"

/* List ACPI tables consumed by the RISC-V clock implementation. */
size_t riscv64_clock_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity);

/* Service a supervisor timer interrupt when one is pending. */
bool clock_handle_irq(const struct exception_frame* frame);
