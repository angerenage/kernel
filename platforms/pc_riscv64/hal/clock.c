#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

#define RISCV64_SIE_STIE (1ull << 5)
#define RISCV64_SCAUSE_INTERRUPT (1ull << 63)
#define RISCV64_SCAUSE_CODE_MASK ((1ull << 63) - 1u)
#define RISCV64_SCAUSE_SUPERVISOR_TIMER 5u

#define RISCV64_QEMU_VIRT_TIMEBASE_HZ 10000000ull
#define RISCV64_SBI_EID_TIME 0x54494d45ul
#define RISCV64_SBI_FID_SET_TIMER 0ul

struct sbi_ret {
	long error;
	long value;
};

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_initialized;
static bool                clock_running;
static uint32_t            clock_frequency_hz;
static uint64_t            clock_interval_ticks;
static uint64_t            clock_next_deadline;
static struct spinlock     clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);

static inline uint64_t read_time(void) {
	uint64_t value;
	__asm__ volatile("csrr %0, time" : "=r"(value));
	return value;
}

static inline uint64_t read_sie(void) {
	uint64_t value;
	__asm__ volatile("csrr %0, sie" : "=r"(value));
	return value;
}

static inline void write_sie(uint64_t value) {
	__asm__ volatile("csrw sie, %0" : : "r"(value) : "memory");
}

static inline void enable_interrupts(void) {
	__asm__ volatile("csrs sstatus, %0" : : "r"(1ull << 1) : "memory");
}

static inline void disable_interrupts(void) {
	__asm__ volatile("csrc sstatus, %0" : : "r"(1ull << 1) : "memory");
}

static struct sbi_ret sbi_call1(unsigned long arg0, unsigned long fid, unsigned long eid) {
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1");
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a7 asm("a7") = eid;

	__asm__ volatile("ecall" : "+r"(a0), "=r"(a1) : "r"(a6), "r"(a7) : "memory", "a2", "a3", "a4", "a5");

	return (struct sbi_ret){
		.error = (long)a0,
		.value = (long)a1,
	};
}

static bool set_timer(uint64_t next_deadline) {
	struct sbi_ret ret = sbi_call1((unsigned long)next_deadline, RISCV64_SBI_FID_SET_TIMER, RISCV64_SBI_EID_TIME);
	return ret.error == 0;
}

static void program_next_deadline(void) {
	uint64_t now = read_time();

	while ((int64_t)(clock_next_deadline - now) <= 0) {
		clock_next_deadline += clock_interval_ticks;
	}

	(void)set_timer(clock_next_deadline);
}

void hal_clock_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	clock_initialized = true;
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint64_t         interval_ticks;
	uint64_t         sie;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	if (clock_running) {
		sie = read_sie();
		sie &= ~RISCV64_SIE_STIE;
		write_sie(sie);
		disable_interrupts();
		(void)set_timer(UINT64_MAX);
		clock_running        = false;
		clock_frequency_hz   = 0u;
		clock_interval_ticks = 0u;
		clock_next_deadline  = 0u;
		clock_handler        = NULL;
		clock_context        = NULL;
	}

	interval_ticks = RISCV64_QEMU_VIRT_TIMEBASE_HZ / frequency_hz;
	if (interval_ticks == 0u) interval_ticks = 1u;

	clock_handler        = handler;
	clock_context        = ctx;
	clock_interval_ticks = interval_ticks;
	clock_next_deadline  = read_time() + clock_interval_ticks;
	clock_frequency_hz   = (uint32_t)(RISCV64_QEMU_VIRT_TIMEBASE_HZ / clock_interval_ticks);
	clock_running        = true;

	if (!set_timer(clock_next_deadline)) {
		sie = read_sie();
		sie &= ~RISCV64_SIE_STIE;
		write_sie(sie);
		disable_interrupts();
		(void)set_timer(UINT64_MAX);
		clock_running        = false;
		clock_frequency_hz   = 0u;
		clock_interval_ticks = 0u;
		clock_next_deadline  = 0u;
		clock_handler        = NULL;
		clock_context        = NULL;
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

	sie = read_sie();
	sie |= RISCV64_SIE_STIE;
	write_sie(sie);
	enable_interrupts();

	printf("kernel: riscv64 clock started (requested=%u Hz, actual=%u Hz, source=sbi time)\n",
	       frequency_hz,
	       clock_frequency_hz);
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
	uint64_t         sie;
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (!clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	sie = read_sie();
	sie &= ~RISCV64_SIE_STIE;
	write_sie(sie);
	disable_interrupts();
	(void)set_timer(UINT64_MAX);

	clock_running        = false;
	clock_frequency_hz   = 0u;
	clock_interval_ticks = 0u;
	clock_next_deadline  = 0u;
	clock_handler        = NULL;
	clock_context        = NULL;
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool clock_handle_irq(const struct exception_frame* frame) {
	bool     is_interrupt;
	uint64_t code;

	if (!clock_running || clock_handler == NULL || frame == NULL) return false;

	is_interrupt = (frame->scause & RISCV64_SCAUSE_INTERRUPT) != 0;
	code         = frame->scause & RISCV64_SCAUSE_CODE_MASK;
	if (!is_interrupt || code != RISCV64_SCAUSE_SUPERVISOR_TIMER) return false;

	program_next_deadline();
	clock_handler(clock_context);
	return true;
}
