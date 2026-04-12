#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	AARCH64_THREAD_CTX_X19 = 0,
	AARCH64_THREAD_CTX_X20,
	AARCH64_THREAD_CTX_X21,
	AARCH64_THREAD_CTX_X22,
	AARCH64_THREAD_CTX_X23,
	AARCH64_THREAD_CTX_X24,
	AARCH64_THREAD_CTX_X25,
	AARCH64_THREAD_CTX_X26,
	AARCH64_THREAD_CTX_X27,
	AARCH64_THREAD_CTX_X28,
	AARCH64_THREAD_CTX_X29,
};

extern void aarch64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void aarch64_thread_entry(void);

uint64_t hal_cpu_boot_arch_id(void) {
	uint64_t value;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
	return value;
}

void* hal_cpu_local_current(void) {
	uint64_t value;

	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));
	return (void*)(uintptr_t)value;
}

void hal_cpu_local_bind(void* ptr) {
	__asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)(uintptr_t)ptr) : "memory");
}

bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg) {
	uintptr_t aligned_top;

	if (context == NULL || entry_pc == 0u || stack_top <= stack_base) return false;

	aligned_top = stack_top & ~(uintptr_t)0xful;
	if (aligned_top <= stack_base) return false;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)aarch64_thread_entry,
		.stack_pointer       = aligned_top,
	};
	context->spill[AARCH64_THREAD_CTX_X19] = entry_pc;
	context->spill[AARCH64_THREAD_CTX_X20] = entry_arg;
	return true;
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	aarch64_thread_context_switch(current, next);
}

void hal_cpu_park(void) {
	__asm__ volatile("wfe" : : : "memory");
}
