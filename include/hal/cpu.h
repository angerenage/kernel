#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu;

/*
 * Architecture hooks for CPU-local storage.
 */

#define HAL_CPU_THREAD_CONTEXT_SPILL_WORDS 16u
#define HAL_CPU_FP_CONTEXT_SIZE 1088u
#define HAL_CPU_FP_CONTEXT_ALIGNMENT 64u
#define HAL_CPU_FP_CONTEXT_STORAGE_SIZE (HAL_CPU_FP_CONTEXT_SIZE + HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u)

/*
 * Opaque eager floating-point/SIMD state owned by one scheduler thread.
 * Internal slack permits aligned architectural access without imposing an
 * over-aligned allocation requirement on struct thread.
 */
struct hal_cpu_fp_context {
	unsigned char opaque[HAL_CPU_FP_CONTEXT_STORAGE_SIZE];
};

_Static_assert((HAL_CPU_FP_CONTEXT_ALIGNMENT & (HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u)) == 0u,
               "FP context alignment must be a power of two");
_Static_assert(sizeof(struct hal_cpu_fp_context) == HAL_CPU_FP_CONTEXT_STORAGE_SIZE,
               "FP context storage size mismatch");

static inline void* hal_cpu_fp_context_data(struct hal_cpu_fp_context* context) {
	uintptr_t address = (uintptr_t)context->opaque;

	return (void*)((address + HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u) & ~(uintptr_t)(HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u));
}

static inline const void* hal_cpu_fp_context_const_data(const struct hal_cpu_fp_context* context) {
	uintptr_t address = (uintptr_t)context->opaque;

	return (const void*)((address + HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u) &
	                     ~(uintptr_t)(HAL_CPU_FP_CONTEXT_ALIGNMENT - 1u));
}

/*
 * HAL-owned thread execution frame.
 *
 * core code may inspect stack/instruction pointers for diagnostics and tests,
 * but the spill area and FP context are architecture-owned state used by
 * context initialization and context switching.
 */
struct thread_context {
	uintptr_t                 instruction_pointer;
	uintptr_t                 stack_pointer;
	uintptr_t                 spill[HAL_CPU_THREAD_CONTEXT_SPILL_WORDS];
	struct hal_cpu_fp_context fp_context;
};

/* Return the hardware identifier for the boot CPU as seen by the active architecture backend. */
uint64_t hal_cpu_boot_arch_id(void);

/* Return the CPU-local pointer previously installed for the running core, or NULL if none is bound yet. */
void* hal_cpu_local_current(void);

/* Bind an arbitrary CPU-local pointer for the running core. */
void hal_cpu_local_bind(void* ptr);

/* Enable the architecture floating-point/SIMD units on the running CPU. */
bool hal_cpu_fp_init(void);

/* Initialize, save, or restore one architecture floating-point/SIMD image. */
void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context);
void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context);
void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context);

/*
 * Initialize a not-yet-run thread frame so the first context switch into it
 * enters entry_pc(entry_arg) on the supplied stack.
 */
bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg);

/* Eagerly switch integer/control and floating-point/SIMD thread state. */
void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next);

/* Prepare the architecture transport used for inter-CPU wake requests before application processors start. */
bool hal_cpu_prepare_smp(void);

/* Park the current CPU in the architecture's low-level idle instruction until the next external wake event. */
void hal_cpu_park(void);

/* Best-effort wake request for cpu so it can observe newly queued work. */
void hal_cpu_kick(const struct cpu* cpu);
