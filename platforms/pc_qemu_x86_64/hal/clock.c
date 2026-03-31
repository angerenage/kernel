#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_running;
static bool                clock_initialized;
static uint32_t            clock_frequency_hz;

void hal_clock_init(void) {
	if (clock_initialized) return;

	interrupts_init_traps();
	clock_initialized = true;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint32_t actual_frequency_hz;

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) return false;

	if (clock_running) hal_clock_stop();

	clock_handler = handler;
	clock_context = ctx;

	if (!pit_init(frequency_hz, &actual_frequency_hz)) {
		clock_handler = NULL;
		clock_context = NULL;
		return false;
	}

	if (!apic_route_isa_irq(0u, X86_IRQ_BASE)) {
		printf("kernel: x86_64 ioapic timer route unavailable, falling back to legacy pic\n");
		pic_unmask_irq(0u);
	}

	clock_running      = true;
	clock_frequency_hz = actual_frequency_hz;
	interrupts_enable();
	printf("kernel: x86_64 clock started (requested=%u Hz, actual=%u Hz, source=pit, route=%s)\n",
	       frequency_hz,
	       clock_frequency_hz,
	       apic_is_active() ? "ioapic/lapic" : "pic");
	return true;
}

uint32_t hal_clock_frequency(void) {
	return clock_frequency_hz;
}

void hal_clock_stop(void) {
	if (!clock_initialized) return;

	clock_running      = false;
	clock_frequency_hz = 0u;
	clock_handler      = NULL;
	clock_context      = NULL;
	interrupts_disable();
}

bool clock_handle_irq(unsigned vector) {
	if (vector != X86_IRQ_BASE || !clock_running || clock_handler == NULL) return false;

	clock_handler(clock_context);
	return true;
}
