#pragma once

#include <stdbool.h>

struct cpu;

/*
 * Minimal interrupt-control surface shared by every architecture backend.
 *
 * The split between global and local init is intentional:
 * - global init installs shared routing/state such as the interrupt descriptor table,
 *   vector tables, or boot-wide controller state
 * - local init finishes per-CPU state such as exception stacks and trap-entry registers
 */

struct irq_state {
	bool enabled;
};

/* Install architecture-wide interrupt state that only needs to be created once during kernel bring-up. */
bool hal_interrupts_init_global(void);

/* Complete interrupt setup for one CPU and mark that CPU as ready to take traps/IRQs. */
bool hal_interrupts_init_local(struct cpu* cpu);

/* Return whether the local CPU currently has maskable interrupts enabled. */
bool irq_enabled(void);

/* Mask interrupts on the current CPU without touching any saved state structure. */
void irq_disable_local(void);

/* Re-enable interrupts on the current CPU. Callers must only do this when the surrounding context allows it. */
void irq_enable_local(void);

/* Snapshot the local interrupt-enable bit, disable interrupts, and update the core nesting counter. */
struct irq_state irq_save_disable(void);

/* Restore the interrupt state previously returned by irq_save_disable(). */
void irq_restore(struct irq_state state);

/* Return true while the current CPU is executing inside a trap/exception handler. */
bool irq_in_exception(void);
