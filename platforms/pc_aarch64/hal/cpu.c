#include <hal/cpu.h>
#include <stdint.h>

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

void hal_cpu_park(void) {
	__asm__ volatile("wfe" : : : "memory");
}
