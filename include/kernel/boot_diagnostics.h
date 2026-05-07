#pragma once

#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool kernel_boot_diagnostics_enabled(void);
void kernel_boot_diagnostics_framebuffer(void);
void kernel_boot_diagnostics_memory_map(const struct mem_range* memory_map, size_t range_count);
void kernel_boot_diagnostics_memory_summary(void);
void kernel_boot_diagnostics_modules(void);
void kernel_boot_diagnostics_scheduler_uptime(uint64_t elapsed_seconds, uint32_t timer_frequency_hz,
                                              size_t* report_lines);
