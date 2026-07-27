#include <core/cpu.h>
#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

#define X86_64_MSR_GS_BASE 0xc0000101u

enum {
	X86_THREAD_STACK_ALIGNMENT  = 16u,
	X86_THREAD_ENTRY_FRAME_SIZE = sizeof(uintptr_t),
	X86_THREAD_CTX_RBX          = 0,
	X86_THREAD_CTX_RBP,
	X86_THREAD_CTX_R12,
	X86_THREAD_CTX_R13,
	X86_THREAD_CTX_R14,
	X86_THREAD_CTX_R15,
};

extern bool x86_64_fp_init(void);
extern void x86_64_fp_context_save(void* context);
extern void x86_64_fp_context_restore(const void* context);
extern void x86_64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void x86_64_thread_entry(void);

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > X86_THREAD_CTX_R15, "x86_64 thread spill area is too small");
_Static_assert(HAL_CPU_FP_CONTEXT_SIZE >= 512u, "x86_64 FXSAVE area is too small");

uint64_t hal_cpu_boot_arch_id(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
	(void)eax;
	(void)ecx;
	(void)edx;
	return (uint64_t)((ebx >> 24) & 0xffu);
}

void* hal_cpu_local_current(void) {
	uint32_t lo;
	uint32_t hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(X86_64_MSR_GS_BASE));
	return (void*)(uintptr_t)(((uint64_t)hi << 32) | lo);
}

void hal_cpu_local_bind(void* ptr) {
	uint64_t value = (uint64_t)(uintptr_t)ptr;
	uint32_t lo    = (uint32_t)value;
	uint32_t hi    = (uint32_t)(value >> 32);

	__asm__ volatile("wrmsr" : : "c"(X86_64_MSR_GS_BASE), "a"(lo), "d"(hi) : "memory");
}

bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg) {
	uintptr_t  aligned_top;
	uintptr_t  initial_sp;
	uintptr_t* return_slot;

	if (context == NULL || entry_pc == 0u || stack_top <= stack_base) return false;

	aligned_top = stack_top & ~((uintptr_t)X86_THREAD_STACK_ALIGNMENT - 1u);
	if (aligned_top <= stack_base) return false;
	if (aligned_top < stack_base + X86_THREAD_ENTRY_FRAME_SIZE) return false;

	/*
	 * x86_64_thread_context_switch restores r12/r13 and jumps into
	 * x86_64_thread_entry, which passes r13 -> rdi and branches to r12.
	 * Seed a synthetic return slot so the first C frame sees the SysV
	 * ABI stack shape (%rsp % 16 == 8 at function entry).
	 */
	initial_sp   = aligned_top - X86_THREAD_ENTRY_FRAME_SIZE;
	return_slot  = (uintptr_t*)initial_sp;
	*return_slot = 0u;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)x86_64_thread_entry,
		.stack_pointer       = initial_sp,
	};
	context->spill[X86_THREAD_CTX_R12] = entry_pc;
	context->spill[X86_THREAD_CTX_R13] = entry_arg;
	hal_cpu_fp_context_init(&context->fp_context);
	return true;
}

bool hal_cpu_fp_init(void) {
	return x86_64_fp_init();
}

void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context) {
	const uint16_t control_word = 0x037fu;
	const uint32_t mxcsr        = 0x1f80u;
	unsigned char* data;

	if (context == NULL) return;
	memset(context, 0, sizeof(*context));
	data = hal_cpu_fp_context_data(context);
	memcpy(&data[0], &control_word, sizeof(control_word));
	memcpy(&data[24], &mxcsr, sizeof(mxcsr));
}

void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	x86_64_fp_context_save(hal_cpu_fp_context_data(context));
}

void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context) {
	if (context == NULL) return;
	x86_64_fp_context_restore(hal_cpu_fp_context_const_data(context));
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	hal_cpu_fp_context_save(&current->fp_context);
	hal_cpu_fp_context_restore(&next->fp_context);
	x86_64_thread_context_switch(current, next);
}

bool hal_cpu_prepare_smp(void) {
	return apic_prepare_ipi();
}

void hal_cpu_park(void) {
	__asm__ volatile("hlt" : : : "memory");
}

void hal_cpu_kick(const struct cpu* cpu) {
	if (cpu == NULL) return;

	(void)apic_send_ipi((uint32_t)cpu->arch_id, X86_LAPIC_WAKE_VECTOR);
}
