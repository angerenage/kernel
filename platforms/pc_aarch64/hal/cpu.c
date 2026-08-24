#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

enum {
	AARCH64_THREAD_STACK_ALIGNMENT = 16u,
	AARCH64_THREAD_CTX_X19         = 0,
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

extern bool aarch64_fp_init(void);
extern void aarch64_fp_context_save(void* context);
extern void aarch64_fp_context_restore(const void* context);
extern void aarch64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void aarch64_thread_entry(void);

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > AARCH64_THREAD_CTX_X29, "aarch64 thread spill area is too small");
_Static_assert(HAL_CPU_FP_CONTEXT_SIZE >= 520u, "aarch64 FP/SIMD area is too small");

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

	/*
	 * aarch64_thread_context_switch restores x19-x29 and branches into
	 * aarch64_thread_entry, which moves x20 -> x0 and branches to x19.
	 * AAPCS64 requires a 16-byte-aligned sp at every public interface.
	 */
	aligned_top = stack_top & ~((uintptr_t)AARCH64_THREAD_STACK_ALIGNMENT - 1u);
	if (aligned_top <= stack_base) return false;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)aarch64_thread_entry,
		.stack_pointer       = aligned_top,
	};
	context->spill[AARCH64_THREAD_CTX_X19] = entry_pc;
	context->spill[AARCH64_THREAD_CTX_X20] = entry_arg;
	hal_cpu_fp_context_init(&context->fp_context);
	return true;
}

bool hal_cpu_fp_init(void) {
	return aarch64_fp_init();
}

void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	memset(context, 0, sizeof(*context));
}

void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	aarch64_fp_context_save(hal_cpu_fp_context_data(context));
}

void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	aarch64_fp_context_restore(hal_cpu_fp_context_const_data(context));
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	hal_cpu_fp_context_save(&current->fp_context);
	hal_cpu_fp_context_restore(&next->fp_context);
	aarch64_thread_context_switch(current, next);
}

bool hal_cpu_prepare_smp(void) {
	return aarch64_gic_prepare_smp();
}

void hal_cpu_park(void) {
	__asm__ volatile("sevl\n\t"
	                 "wfe\n\t"
	                 "wfe"
	                 :
	                 :
	                 : "memory");
}

void hal_cpu_kick(const struct cpu* cpu) {
	if (cpu == NULL) return;

	__asm__ volatile("dsb ishst\n\t"
	                 "sev"
	                 :
	                 :
	                 : "memory");
}
