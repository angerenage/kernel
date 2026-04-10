#include <core/cpu.h>
#include <hal/interrupts.h>

static _Thread_local bool hosted_irq_enabled = true;

bool irq_enabled(void) {
	return hosted_irq_enabled;
}

void irq_disable_local(void) {
	hosted_irq_enabled = false;
}

void irq_enable_local(void) {
	hosted_irq_enabled = true;
}

bool hal_interrupts_init_global(void) {
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	if (cpu == NULL) return false;

	cpu_interrupts_set_ready(cpu, true);
	return true;
}
