#include "clock.h"

#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/clock.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

#include "interrupts/gic.h"

#define AARCH64_CNTV_CTL_ENABLE (1u << 0)
#define AARCH64_TIMER_PPI 27u

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_initialized;
static bool                clock_running;
static uint32_t            clock_frequency_hz;
static uint64_t            clock_interval_ticks;
static uint64_t            clock_next_deadline;
static struct spinlock     clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
static struct hal_interrupt_source_state timer_source;

static inline uint64_t read_counter_frequency(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

static inline uint64_t read_counter(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
	return value;
}

static inline void write_deadline(uint64_t value) {
	__asm__ volatile("msr cntv_cval_el0, %0" : : "r"(value) : "memory");
}

static inline void write_timer_control(uint32_t value) {
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)value) : "memory");
}

bool aarch64_clock_fire(void) {
	if (!clock_running || clock_handler == NULL) return false;
	uint64_t now = read_counter();
	while ((int64_t)(clock_next_deadline - now) <= 0) clock_next_deadline += clock_interval_ticks;
	write_deadline(clock_next_deadline);
	__asm__ volatile("isb" : : : "memory");
	clock_handler(clock_context);
	return true;
}

void hal_clock_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);
	if (!clock_initialized) {
		write_timer_control(0u);
		clock_initialized = true;
	}
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint64_t         counter_hz;
	uint64_t         interval_ticks;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);
	if (!clock_initialized || frequency_hz == 0u || handler == NULL) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	if (!aarch64_gic_local_source_deinit(&timer_source)) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	write_timer_control(0u);
	clock_running = false;
	counter_hz    = read_counter_frequency();
	if (counter_hz == 0u) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	interval_ticks = counter_hz / frequency_hz;
	if (interval_ticks == 0u) interval_ticks = 1u;
	if (!aarch64_gic_local_source_init(&timer_source, AARCH64_TIMER_PPI, cpu_current())) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	clock_handler        = handler;
	clock_context        = ctx;
	clock_interval_ticks = interval_ticks;
	clock_next_deadline  = read_counter() + interval_ticks;
	clock_frequency_hz   = (uint32_t)(counter_hz / interval_ticks);
	clock_running        = true;
	write_deadline(clock_next_deadline);
	write_timer_control(AARCH64_CNTV_CTL_ENABLE);
	if (!aarch64_gic_local_source_unmask(&timer_source)) {
		if (aarch64_gic_local_source_deinit(&timer_source)) {
			write_timer_control(0u);
			clock_running = false;
		}
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	spinlock_unlock_irqrestore(&clock_lock, state);
	return true;
}

uint32_t hal_clock_frequency(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);
	uint32_t         hz    = clock_frequency_hz;
	spinlock_unlock_irqrestore(&clock_lock, state);
	return hz;
}

void hal_clock_stop(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);
	if (clock_initialized) {
		if (aarch64_gic_local_source_deinit(&timer_source)) {
			write_timer_control(0u);
			clock_running        = false;
			clock_frequency_hz   = 0u;
			clock_interval_ticks = 0u;
			clock_next_deadline  = 0u;
			clock_handler        = NULL;
			clock_context        = NULL;
		}
	}
	spinlock_unlock_irqrestore(&clock_lock, state);
}
