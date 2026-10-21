#include "clock.h"

#include <boot/info.h>
#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/clock.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts/pic.h"
#include "interrupts/vectors.h"

static hal_clock_handler_t               clock_handler;
static void*                             clock_context;
static bool                              clock_running;
static bool                              clock_initialized;
static struct hal_interrupt_source_state timer_source;
static uint32_t                          clock_frequency_hz;
static uint32_t                          clock_vector = UINT32_MAX;
static uint64_t                          monotonic_frequency_hz;
static struct spinlock clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);

static bool invariant_tsc_available(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u), "c"(0u));
	if (eax < 0x80000007u) return false;
	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000007u), "c"(0u));
	return (edx & (1u << 8)) != 0u;
}

static uint64_t read_tsc(void) {
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(low), "=d"(high) : : "memory");
	return ((uint64_t)high << 32u) | low;
}

static void clock_reset_state(void) {
	__atomic_store_n(&clock_running, false, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_frequency_hz, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_vector, UINT32_MAX, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_handler, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_context, NULL, __ATOMIC_RELEASE);
}

void hal_clock_init(void) {
	struct irq_state        state = spinlock_lock_irqsave(&clock_lock);
	const struct boot_info* info;

	if (clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	clock_reset_state();
	info                   = boot_info_get();
	monotonic_frequency_hz = info != NULL && invariant_tsc_available() ? info->monotonic_counter_frequency_hz : 0u;
	clock_initialized      = true;
	spinlock_unlock_irqrestore(&clock_lock, state);
}

uint64_t hal_clock_monotonic_frequency(void) {
	return __atomic_load_n(&monotonic_frequency_hz, __ATOMIC_ACQUIRE);
}

bool hal_clock_monotonic_counter(uint64_t* out_counter) {
	if (out_counter == NULL || hal_clock_monotonic_frequency() == 0u) return false;
	*out_counter = read_tsc();
	return true;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint32_t         actual_frequency_hz;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	if (clock_running) {
		if (!hal_interrupt_source_deinit(&timer_source)) {
			spinlock_unlock_irqrestore(&clock_lock, state);
			return false;
		}
		clock_reset_state();
	}

	if (!pit_init(frequency_hz, &actual_frequency_hz)) {
		clock_reset_state();
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	struct hal_interrupt_source      source = {.domain = 0u, .number = 0u};
	struct hal_interrupt_source_info source_info;
	if (!hal_interrupt_source_info(&source, &source_info)) {
		clock_reset_state();
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	struct hal_interrupt_delivery delivery = {
		.target   = cpu_current(),
		.event    = {.domain = source_info.delivery.domain, .id = source_info.delivery.base},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE,
	};
	if (!hal_interrupt_source_init(&timer_source, &source, &delivery)) {
		clock_reset_state();
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	__atomic_store_n(&clock_vector, delivery.event.id, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_handler, handler, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_context, ctx, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_frequency_hz, actual_frequency_hz, __ATOMIC_RELEASE);
	__atomic_store_n(&clock_running, true, __ATOMIC_RELEASE);
	if (!hal_interrupt_source_unmask(&timer_source)) {
		if (hal_interrupt_source_deinit(&timer_source)) clock_reset_state();
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
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

	if (hal_interrupt_source_deinit(&timer_source)) clock_reset_state();
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool clock_handle_irq(unsigned vector) {
	hal_clock_handler_t handler;
	void*               ctx;

	if (vector != __atomic_load_n(&clock_vector, __ATOMIC_ACQUIRE)) return false;
	if (!__atomic_load_n(&clock_running, __ATOMIC_ACQUIRE)) return true;

	handler = __atomic_load_n(&clock_handler, __ATOMIC_ACQUIRE);
	if (handler == NULL) return true;

	ctx = __atomic_load_n(&clock_context, __ATOMIC_ACQUIRE);

	handler(ctx);
	return true;
}
