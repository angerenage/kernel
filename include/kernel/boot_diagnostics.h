#pragma once

#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return true when boot-time diagnostic logging is enabled for the current image. */
bool kernel_boot_diagnostics_enabled(void);

/* Print the framebuffer description captured during kernel_boot_init(), when available. */
void kernel_boot_diagnostics_framebuffer(void);

/* Print a per-range breakdown of the bootloader-supplied memory map. */
void kernel_boot_diagnostics_memory_map(const struct mem_range* memory_map, size_t range_count);

/* Print a high-level summary of usable/reusable/reserved memory ranges. */
void kernel_boot_diagnostics_memory_summary(void);

/* Print the list of boot modules the bootloader reported. */
void kernel_boot_diagnostics_modules(void);

/* Append a scheduler utilization report computed from the interval since the previous report. */
void kernel_boot_diagnostics_scheduler_report(uint64_t elapsed_ticks, uint32_t timer_frequency_hz);
