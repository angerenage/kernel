#pragma once

#include <core/cpu.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Higher-level CPU bring-up helpers layered on top of kernel_boot_* and the
 * core CPU topology code.
 */

/* Discover the boot CPU topology, assign application processor bootstrap stacks, and populate core cpu_topology state.
 */
bool kernel_cpu_boot_init(uintptr_t boot_stack_base, uintptr_t boot_stack_top);

/* Bind struct cpu to the running hardware thread and record whether cpu_current() resolves correctly afterward. */
void kernel_cpu_boot_bind_current(struct cpu* cpu);

/* Start every discovered application processor and wait for each one to transition to CPU_STATE_ONLINE. */
bool kernel_cpu_boot_start_aps(void);

/* Return whether a CPU successfully observed its own struct cpu pointer through the HAL CPU-local register. */
bool kernel_cpu_boot_current_pointer_ok(const struct cpu* cpu);
