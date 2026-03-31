#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static bool clock_initialized;
extern void riscv64_exception_entry(void);

void hal_clock_init(void) {
	if (clock_initialized) return;

	uintptr_t entry    = (uintptr_t)riscv64_exception_entry;
	uintptr_t zero     = 0;
	uintptr_t sie_mask = 1u << 1;

	__asm__ volatile("csrw stvec, %0" : : "r"(entry) : "memory");
	__asm__ volatile("csrw sie, %0" : : "r"(zero) : "memory");
	__asm__ volatile("csrc sstatus, %0" : : "r"(sie_mask) : "memory");

	clock_initialized = true;
	printf("kernel: riscv64 trap vector installed\n");
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
