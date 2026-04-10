#pragma once

#include <stdint.h>

/*
 * Architecture hooks for CPU-local storage.
 */

/* Return the hardware identifier for the boot CPU as seen by the active architecture backend. */
uint64_t hal_cpu_boot_arch_id(void);

/* Return the CPU-local pointer previously installed for the running core, or NULL if none is bound yet. */
void* hal_cpu_local_current(void);

/* Bind an arbitrary CPU-local pointer for the running core. */
void hal_cpu_local_bind(void* ptr);

/* Park the current CPU in the architecture's low-level idle instruction until the next external wake event. */
void hal_cpu_park(void);
