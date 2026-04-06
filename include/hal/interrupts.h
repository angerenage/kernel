#pragma once

#include <stdbool.h>

struct cpu;

struct irq_state {
	bool enabled;
};

bool             hal_interrupts_init_global(void);
bool             hal_interrupts_init_local(struct cpu* cpu);
bool             irq_enabled(void);
void             irq_disable_local(void);
void             irq_enable_local(void);
struct irq_state irq_save_disable(void);
void             irq_restore(struct irq_state state);
bool             irq_in_exception(void);
