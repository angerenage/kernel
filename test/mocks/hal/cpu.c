#include <core/cpu.h>
#include <hal/cpu.h>
#include <stddef.h>

#include "cpu_mock.h"

static _Thread_local void*                hosted_cpu_local_ptr;
static hal_cpu_mock_context_switch_hook_t hosted_context_switch_hook;
static size_t                             hosted_kick_count[64];

uint64_t hal_cpu_boot_arch_id(void) {
	return 0u;
}

void* hal_cpu_local_current(void) {
	return hosted_cpu_local_ptr;
}

void hal_cpu_local_bind(void* ptr) {
	hosted_cpu_local_ptr = ptr;
}

bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg) {
	if (context == NULL || entry_pc == 0u || stack_top <= stack_base) return false;

	*context = (struct thread_context){
		.instruction_pointer = entry_pc,
		.stack_pointer       = stack_top,
	};
	context->spill[0] = entry_arg;
	return true;
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (hosted_context_switch_hook != NULL) hosted_context_switch_hook(current, next);

	(void)current;
	(void)next;
}

void hal_cpu_park(void) {
}

void hal_cpu_kick(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= 64u) return;

	hosted_kick_count[cpu->index]++;
}

void hal_cpu_mock_set_context_switch_hook(hal_cpu_mock_context_switch_hook_t hook) {
	hosted_context_switch_hook = hook;
}

void hal_cpu_mock_reset_kicks(void) {
	for (size_t i = 0; i < 64u; i++) {
		hosted_kick_count[i] = 0u;
	}
}

size_t hal_cpu_mock_kick_count(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= 64u) return 0u;

	return hosted_kick_count[cpu->index];
}
