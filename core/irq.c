#include <core/cpu.h>
#include <hal/interrupts.h>

struct irq_state irq_save_disable(void) {
	struct irq_state state = {
		.enabled = irq_enabled(),
	};
	struct cpu* cpu = cpu_current();

	irq_disable_local();
	if (cpu != NULL) cpu->irq_disable_depth++;
	return state;
}

void irq_restore(struct irq_state state) {
	struct cpu* cpu = cpu_current();

	if (cpu != NULL && cpu->irq_disable_depth != 0u) cpu->irq_disable_depth--;
	if (state.enabled) irq_enable_local();
}

bool irq_in_exception(void) {
	return cpu_irq_in_exception();
}
