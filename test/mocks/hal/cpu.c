#include <hal/cpu.h>

static _Thread_local void* hosted_cpu_local_ptr;

uint64_t hal_cpu_boot_arch_id(void) {
	return 0u;
}

void* hal_cpu_local_current(void) {
	return hosted_cpu_local_ptr;
}

void hal_cpu_local_bind(void* ptr) {
	hosted_cpu_local_ptr = ptr;
}

void hal_cpu_park(void) {
}
