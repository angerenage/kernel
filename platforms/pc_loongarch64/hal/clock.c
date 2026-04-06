#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

#define LOONGARCH64_CSR_CRMD 0x0u
#define LOONGARCH64_CSR_ECFG 0x4u
#define LOONGARCH64_CSR_TCFG 0x41u
#define LOONGARCH64_CSR_TICLR 0x44u

#define LOONGARCH64_CRMD_IE (1u << 2)
#define LOONGARCH64_TIMER_INT_BIT 11u
#define LOONGARCH64_TIMER_INT_MASK (1u << LOONGARCH64_TIMER_INT_BIT)
#define LOONGARCH64_TCFG_ENABLE (1u << 0)
#define LOONGARCH64_TCFG_PERIODIC (1u << 1)
#define LOONGARCH64_TICLR_CLEAR 1u
#define LOONGARCH64_TIMER_MIN_INTERVAL 4ull

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_initialized;
static bool                clock_running;
static uint32_t            clock_frequency_hz;

static inline uint32_t cpucfg_word(unsigned word) {
	uint64_t value;
	__asm__ volatile("cpucfg %0, %1" : "=r"(value) : "r"((uint64_t)word));
	return (uint32_t)value;
}

static inline uint64_t csrrd(unsigned csr) {
	uint64_t value;

	switch (csr) {
	case LOONGARCH64_CSR_CRMD:
		__asm__ volatile("csrrd %0, 0x0" : "=r"(value));
		break;
	case LOONGARCH64_CSR_ECFG:
		__asm__ volatile("csrrd %0, 0x4" : "=r"(value));
		break;
	case LOONGARCH64_CSR_TCFG:
		__asm__ volatile("csrrd %0, 0x41" : "=r"(value));
		break;
	default:
		return 0;
	}

	return value;
}

static inline void csrwr(uint64_t value, unsigned csr) {
	switch (csr) {
	case LOONGARCH64_CSR_CRMD:
		__asm__ volatile("csrwr %0, 0x0" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_ECFG:
		__asm__ volatile("csrwr %0, 0x4" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_TCFG:
		__asm__ volatile("csrwr %0, 0x41" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_TICLR:
		__asm__ volatile("csrwr %0, 0x44" : : "r"(value) : "memory");
		break;
	default:
		break;
	}
}

static uint64_t timer_frequency_hz(void) {
	uint64_t base_frequency = cpucfg_word(4u);
	uint64_t scale          = cpucfg_word(5u);
	uint64_t multiplier     = scale & 0xffffu;
	uint64_t divisor        = scale >> 16;

	if (base_frequency == 0u) return 0u;
	if (multiplier == 0u) multiplier = 1u;
	if (divisor == 0u) divisor = 1u;

	return (base_frequency * multiplier) / divisor;
}

void hal_clock_init(void) {
	if (clock_initialized) return;
	csrwr(0u, LOONGARCH64_CSR_TCFG);
	csrwr(LOONGARCH64_TICLR_CLEAR, LOONGARCH64_CSR_TICLR);

	clock_initialized = true;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint64_t timer_hz;
	uint64_t interval_ticks;
	uint64_t ecfg;
	uint64_t crmd;

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) return false;

	if (clock_running) hal_clock_stop();

	timer_hz = timer_frequency_hz();
	if (timer_hz == 0u) return false;

	interval_ticks = timer_hz / frequency_hz;
	if (interval_ticks < LOONGARCH64_TIMER_MIN_INTERVAL) interval_ticks = LOONGARCH64_TIMER_MIN_INTERVAL;
	interval_ticks = (interval_ticks + (LOONGARCH64_TIMER_MIN_INTERVAL - 1u)) & ~(LOONGARCH64_TIMER_MIN_INTERVAL - 1u);

	clock_handler      = handler;
	clock_context      = ctx;
	clock_frequency_hz = (uint32_t)(timer_hz / interval_ticks);
	clock_running      = true;

	csrwr(LOONGARCH64_TICLR_CLEAR, LOONGARCH64_CSR_TICLR);
	csrwr(interval_ticks | LOONGARCH64_TCFG_PERIODIC | LOONGARCH64_TCFG_ENABLE, LOONGARCH64_CSR_TCFG);

	ecfg = csrrd(LOONGARCH64_CSR_ECFG);
	ecfg |= LOONGARCH64_TIMER_INT_MASK;
	csrwr(ecfg, LOONGARCH64_CSR_ECFG);

	crmd = csrrd(LOONGARCH64_CSR_CRMD);
	crmd |= LOONGARCH64_CRMD_IE;
	csrwr(crmd, LOONGARCH64_CSR_CRMD);

	printf("kernel: loongarch64 clock started (requested=%u Hz, actual=%u Hz, source=csr timer)\n",
	       frequency_hz,
	       clock_frequency_hz);
	return true;
}

uint32_t hal_clock_frequency(void) {
	return clock_frequency_hz;
}

void hal_clock_stop(void) {
	uint64_t ecfg;
	uint64_t crmd;

	if (!clock_initialized) return;

	csrwr(0u, LOONGARCH64_CSR_TCFG);
	csrwr(LOONGARCH64_TICLR_CLEAR, LOONGARCH64_CSR_TICLR);

	ecfg = csrrd(LOONGARCH64_CSR_ECFG);
	ecfg &= ~LOONGARCH64_TIMER_INT_MASK;
	csrwr(ecfg, LOONGARCH64_CSR_ECFG);

	crmd = csrrd(LOONGARCH64_CSR_CRMD);
	crmd &= ~LOONGARCH64_CRMD_IE;
	csrwr(crmd, LOONGARCH64_CSR_CRMD);

	clock_running      = false;
	clock_frequency_hz = 0u;
	clock_handler      = NULL;
	clock_context      = NULL;
}

bool clock_handle_irq(const struct exception_frame* frame) {
	uint64_t ecode;

	if (!clock_running || clock_handler == NULL || frame == NULL) return false;

	ecode = (frame->estat >> 16) & 0x3fu;
	if (ecode != 0u || (frame->estat & LOONGARCH64_TIMER_INT_MASK) == 0u) return false;

	csrwr(LOONGARCH64_TICLR_CLEAR, LOONGARCH64_CSR_TICLR);
	clock_handler(clock_context);
	return true;
}
