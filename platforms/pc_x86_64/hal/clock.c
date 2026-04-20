#include <core/cpu.h>
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
	__atomic_store_n(&clock_apic_routed, false, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_running, false, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_frequency_hz, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_handler, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_context, NULL, __ATOMIC_RELEASE);
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

	__atomic_store_n(&clock_handler, handler, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_context, ctx, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_frequency_hz, actual_frequency_hz, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_running, true, __ATOMIC_RELEASE);
	const char* route_name = clock_enable_timer_irq();
	printf("kernel: x86_64 clock started (requested=%u Hz, actual=%u Hz, source=pit, route=%s)\n",
	       frequency_hz,
	       __atomic_load_n(&clock_frequency_hz, __ATOMIC_ACQUIRE),
	       route_name);
	spinlock_unlock_irqrestore(&clock_lock, state);
	return true;
}

uint32_t hal_clock_frequency(void) {
	uint32_t         hz;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	hz = __atomic_load_n(&clock_frequency_hz, __ATOMIC_ACQUIRE);
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
	hal_clock_handler_t handler;
	void*               ctx;

	if (vector != X86_IRQ_BASE) return false;
	if (!__atomic_load_n(&clock_running, __ATOMIC_ACQUIRE)) return true;

	handler = __atomic_load_n(&clock_handler, __ATOMIC_ACQUIRE);
	if (handler == NULL) return true;

	ctx = __atomic_load_n(&clock_context, __ATOMIC_ACQUIRE);

	handler(ctx);
	return true;
}
