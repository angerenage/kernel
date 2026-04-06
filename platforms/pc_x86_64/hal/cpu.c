#include <hal/cpu.h>
#include <stdint.h>

#define X86_64_MSR_GS_BASE 0xc0000101u

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

void hal_cpu_park(void) {
	__asm__ volatile("hlt" : : : "memory");
}
