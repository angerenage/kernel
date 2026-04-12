#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X86_64_MSR_GS_BASE 0xc0000101u

enum {
	X86_THREAD_CTX_RBX = 0,
	X86_THREAD_CTX_RBP,
	X86_THREAD_CTX_R12,
	X86_THREAD_CTX_R13,
	X86_THREAD_CTX_R14,
	X86_THREAD_CTX_R15,
};

extern void x86_64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void x86_64_thread_entry(void);

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

	aligned_top = stack_top & ~(uintptr_t)0xful;
	if (aligned_top <= stack_base) return false;
	if (aligned_top < stack_base + sizeof(uintptr_t)) return false;

	initial_sp   = aligned_top - sizeof(uintptr_t);
	return_slot  = (uintptr_t*)initial_sp;
	*return_slot = 0u;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)x86_64_thread_entry,
		.stack_pointer       = initial_sp,
	};
	context->spill[X86_THREAD_CTX_R12] = entry_pc;
	context->spill[X86_THREAD_CTX_R13] = entry_arg;
	return true;
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	x86_64_thread_context_switch(current, next);
}

void hal_cpu_park(void) {
	__asm__ volatile("hlt" : : : "memory");
}
