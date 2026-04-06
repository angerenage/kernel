#include <hal/cpu.h>
#include <stdint.h>

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
