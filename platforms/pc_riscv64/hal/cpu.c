#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RISCV64_THREAD_CTX_S0 = 0,
	RISCV64_THREAD_CTX_S1,
	RISCV64_THREAD_CTX_S2,
	RISCV64_THREAD_CTX_S3,
	RISCV64_THREAD_CTX_S4,
	RISCV64_THREAD_CTX_S5,
	RISCV64_THREAD_CTX_S6,
	RISCV64_THREAD_CTX_S7,
	RISCV64_THREAD_CTX_S8,
	RISCV64_THREAD_CTX_S9,
	RISCV64_THREAD_CTX_S10,
	RISCV64_THREAD_CTX_S11,
};

extern void riscv64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void riscv64_thread_entry(void);

uint64_t hal_cpu_boot_arch_id(void) {
	return 0u;
}

void* hal_cpu_local_current(void) {
	uintptr_t value;

	__asm__ volatile("mv %0, tp" : "=r"(value));
	return (void*)value;
}

void hal_cpu_local_bind(void* ptr) {
	__asm__ volatile("mv tp, %0" : : "r"((uintptr_t)ptr) : "memory");
}

bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg) {
	uintptr_t aligned_top;

	if (context == NULL || entry_pc == 0u || stack_top <= stack_base) return false;

	aligned_top = stack_top & ~(uintptr_t)0xful;
	if (aligned_top <= stack_base) return false;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)riscv64_thread_entry,
		.stack_pointer       = aligned_top,
	};
	context->spill[RISCV64_THREAD_CTX_S0] = entry_pc;
	context->spill[RISCV64_THREAD_CTX_S1] = entry_arg;
	return true;
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	riscv64_thread_context_switch(current, next);
}

void hal_cpu_park(void) {
	__asm__ volatile("wfi" : : : "memory");
}
