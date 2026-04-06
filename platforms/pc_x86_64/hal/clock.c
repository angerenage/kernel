#include <core/lock.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_running;
static bool                clock_initialized;
static bool                clock_apic_routed;
static uint32_t            clock_frequency_hz;
static struct spinlock     clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);

static void clock_reset_state(void) {
	clock_apic_routed  = false;
	clock_running      = false;
	clock_frequency_hz = 0u;
	clock_handler      = NULL;
	clock_context      = NULL;
}

static const char* clock_enable_timer_irq(void) {
	clock_apic_routed = apic_route_isa_irq(0u, X86_IRQ_BASE);
	if (clock_apic_routed) {
		(void)apic_set_isa_irq_mask(0u, false);
		return "ioapic/lapic";
	}

	printf("kernel: x86_64 ioapic timer route unavailable, falling back to legacy pic\n");
	pic_unmask_irq(0u);
	return "pic";
}

static void clock_disable_timer_irq(void) {
	if (clock_apic_routed) {
		(void)apic_set_isa_irq_mask(0u, true);
		return;
	}

	pic_mask_irq(0u);
}

void hal_clock_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	clock_reset_state();
	clock_initialized = true;
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint32_t         actual_frequency_hz;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	if (clock_running) {
		clock_disable_timer_irq();
		clock_reset_state();
	}

	if (!pit_init(frequency_hz, &actual_frequency_hz)) {
		clock_reset_state();
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	clock_handler          = handler;
	clock_context          = ctx;
	clock_running          = true;
	clock_frequency_hz     = actual_frequency_hz;
	const char* route_name = clock_enable_timer_irq();
	printf("kernel: x86_64 clock started (requested=%u Hz, actual=%u Hz, source=pit, route=%s)\n",
	       frequency_hz,
	       clock_frequency_hz,
	       route_name);
	spinlock_unlock_irqrestore(&clock_lock, state);
	return true;
}

uint32_t hal_clock_frequency(void) {
	uint32_t         hz;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	hz = clock_frequency_hz;
	spinlock_unlock_irqrestore(&clock_lock, state);
	return hz;
}

void hal_clock_stop(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (!clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	clock_disable_timer_irq();
	clock_reset_state();
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool clock_handle_irq(unsigned vector) {
	if (vector != X86_IRQ_BASE || !clock_running || clock_handler == NULL) return false;

	clock_handler(clock_context);
	return true;
}
