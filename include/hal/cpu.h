#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Architecture hooks for CPU-local storage.
 */

#define HAL_CPU_THREAD_CONTEXT_SPILL_WORDS 16u

/*
 * HAL-owned thread execution frame.
 *
 * core code may inspect stack/instruction pointers for diagnostics and tests,
 * but the spill area is architecture-defined scratch used by context init and
 * context switch implementations.
 */
struct thread_context {
	uintptr_t instruction_pointer;
	uintptr_t stack_pointer;
	uintptr_t spill[HAL_CPU_THREAD_CONTEXT_SPILL_WORDS];
};

/* Return the hardware identifier for the boot CPU as seen by the active architecture backend. */
uint64_t hal_cpu_boot_arch_id(void);

/* Return the CPU-local pointer previously installed for the running core, or NULL if none is bound yet. */
void* hal_cpu_local_current(void);

/* Bind an arbitrary CPU-local pointer for the running core. */
void hal_cpu_local_bind(void* ptr);

/*
 * Initialize a not-yet-run thread frame so the first context switch into it
 * enters entry_pc(entry_arg) on the supplied stack.
 */
bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg);

/* Save the current thread frame and resume the target frame. */
void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next);

/* Park the current CPU in the architecture's low-level idle instruction until the next external wake event. */
void hal_cpu_park(void);
