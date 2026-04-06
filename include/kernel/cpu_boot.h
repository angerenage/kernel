#pragma once

#include <core/cpu.h>
#include <stdbool.h>
#include <stdint.h>

bool kernel_cpu_boot_init(uintptr_t boot_stack_base, uintptr_t boot_stack_top);
void kernel_cpu_boot_bind_current(struct cpu* cpu);
bool kernel_cpu_boot_start_aps(void);
bool kernel_cpu_boot_current_pointer_ok(const struct cpu* cpu);
