#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static bool clock_initialized;
extern char aarch64_exception_vectors[];

void hal_clock_init(void) {
	if (clock_initialized) return;

	uintptr_t vectors = (uintptr_t)aarch64_exception_vectors;

	__asm__ volatile("msr vbar_el1, %0\n\t"
	                 "isb"
	                 :
	                 : "r"(vectors)
	                 : "memory");

	clock_initialized = true;
	printf("kernel: aarch64 vectors installed\n");
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	(void)frequency_hz;
	(void)handler;
	(void)ctx;

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) return false;
	return false;
}

uint32_t hal_clock_frequency(void) {
	return 0u;
}

void hal_clock_stop(void) {
}
