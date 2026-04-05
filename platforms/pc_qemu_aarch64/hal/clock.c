#include <hal/clock.h>
#include <hal/paging.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

#define AARCH64_GICD_BASE_PHYS 0x08000000ull
#define AARCH64_GICC_BASE_PHYS 0x08010000ull
#define AARCH64_MMIO_PAGE_SIZE 0x1000u

#define AARCH64_GICD_CTLR 0x000u
#define AARCH64_GICD_IGROUPR0 0x080u
#define AARCH64_GICD_ISENABLER0 0x100u
#define AARCH64_GICD_ICENABLER0 0x180u
#define AARCH64_GICD_IPRIORITYR 0x400u

#define AARCH64_GICC_CTLR 0x000u
#define AARCH64_GICC_PMR 0x004u

#define AARCH64_GICD_CTLR_ENABLE_GRP1 (1u << 1)
#define AARCH64_GICC_CTLR_ENABLE_GRP1 (1u << 1)

#define AARCH64_CNTV_CTL_ENABLE (1u << 0)

static hal_clock_handler_t clock_handler;
static void*               clock_context;
static bool                clock_initialized;
static bool                clock_running;
static bool                gic_ready;
static uint32_t            clock_frequency_hz;
static uint64_t            clock_interval_ticks;
static uint64_t            clock_next_deadline;
static volatile uint8_t*   gicd_mmio;
static volatile uint8_t*   gicc_mmio;

static inline uintptr_t phys_to_virt(uintptr_t phys) {
	return (uintptr_t)(hhdm_req.response->offset + phys);
}

static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint32_t*)(base + offset);
}

static inline void mmio_write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
	*(volatile uint32_t*)(base + offset) = value;
}

static inline void mmio_write8(volatile uint8_t* base, uint32_t offset, uint8_t value) {
	*(volatile uint8_t*)(base + offset) = value;
}

static inline void sync(void) {
	__asm__ volatile("dsb sy\n\t"
	                 "isb" ::
	                     : "memory");
}

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

static inline void mask_irqs(void) {
	__asm__ volatile("msr daifset, #2" : : : "memory");
}

static inline void unmask_irqs(void) {
	__asm__ volatile("msr daifclr, #2" : : : "memory");
}

static bool map_mmio_page(uintptr_t phys) {
	uintptr_t page_phys = phys & ~(uintptr_t)(AARCH64_MMIO_PAGE_SIZE - 1u);
	uintptr_t page_virt;
	uintptr_t existing_phys = 0;

	if (hhdm_req.response == NULL) return false;

	page_virt = phys_to_virt(page_phys);
	if (hal_paging_query(page_virt, &existing_phys, NULL)) return true;

	return hal_paging_map(page_virt, page_phys, HAL_PAGE_WRITE | HAL_PAGE_GLOBAL | HAL_PAGE_NO_CACHE);
}

static bool gic_init(void) {
	if (gic_ready) return true;

	if (!map_mmio_page(AARCH64_GICD_BASE_PHYS) || !map_mmio_page(AARCH64_GICC_BASE_PHYS)) {
		return false;
	}

	gicd_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(AARCH64_GICD_BASE_PHYS);
	gicc_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(AARCH64_GICC_BASE_PHYS);

	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, 0u);
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0, mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0) | (1u << 27));
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + 27u, 0x80u);
	mmio_write32(gicd_mmio, AARCH64_GICD_ICENABLER0, 1u << 27);
	mmio_write32(gicd_mmio, AARCH64_GICD_ISENABLER0, 1u << 27);
	mmio_write32(gicc_mmio, AARCH64_GICC_PMR, 0xffu);
	mmio_write32(gicc_mmio, AARCH64_GICC_CTLR, AARCH64_GICC_CTLR_ENABLE_GRP1);
	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, AARCH64_GICD_CTLR_ENABLE_GRP1);
	sync();

	gic_ready = true;
	return true;
}

static bool is_irq_vector(uint64_t vector) {
	switch (vector) {
	case 1u:
	case 5u:
	case 9u:
	case 13u:
		return true;
	default:
		return false;
	}
}

static void program_next_deadline(void) {
	uint64_t now = read_counter();

	while ((int64_t)(clock_next_deadline - now) <= 0) {
		clock_next_deadline += clock_interval_ticks;
	}

	write_deadline(clock_next_deadline);
	__asm__ volatile("isb" : : : "memory");
}

void hal_clock_init(void) {
	if (clock_initialized) return;

	write_timer_control(0u);

	clock_initialized = true;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint64_t counter_hz;
	uint64_t interval_ticks;

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) return false;

	if (clock_running) hal_clock_stop();
	if (!gic_init()) return false;

	counter_hz = read_counter_frequency();
	if (counter_hz == 0u) return false;

	interval_ticks = counter_hz / frequency_hz;
	if (interval_ticks == 0u) interval_ticks = 1u;

	clock_handler        = handler;
	clock_context        = ctx;
	clock_interval_ticks = interval_ticks;
	clock_next_deadline  = read_counter() + clock_interval_ticks;
	clock_frequency_hz   = (uint32_t)(counter_hz / clock_interval_ticks);
	clock_running        = true;

	write_deadline(clock_next_deadline);
	write_timer_control(AARCH64_CNTV_CTL_ENABLE);
	unmask_irqs();

	printf("kernel: aarch64 clock started (requested=%u Hz, actual=%u Hz, source=generic timer)\n",
	       frequency_hz,
	       clock_frequency_hz);
	return true;
}

uint32_t hal_clock_frequency(void) {
	return clock_frequency_hz;
}

void hal_clock_stop(void) {
	if (!clock_initialized) return;

	mask_irqs();
	write_timer_control(0u);

	clock_running        = false;
	clock_frequency_hz   = 0u;
	clock_interval_ticks = 0u;
	clock_next_deadline  = 0u;
	clock_handler        = NULL;
	clock_context        = NULL;
}

bool clock_handle_irq(const struct exception_frame* frame) {
	if (!clock_running || clock_handler == NULL || frame == NULL || !is_irq_vector(frame->vector) || !gic_ready) {
		return false;
	}

	/*
	 * The current QEMU/EDK2 AArch64 boot path delivers the timer IRQ once the
	 * PPI is unmasked through the GIC MMIO window, but the obvious acknowledge
	 * paths either trap at EL1 or report spurious IDs. Re-arming the timer is
	 * enough to drop the line on this setup, so keep the handler minimal here.
	 */
	program_next_deadline();
	clock_handler(clock_context);
	return true;
}
