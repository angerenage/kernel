#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/clock.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts_private.h"

#define AARCH64_GICD_BASE_PHYS 0x08000000ull
#define AARCH64_GICC_BASE_PHYS 0x08010000ull
#define AARCH64_MMIO_PAGE_SIZE 0x1000u
#define AARCH64_GIC_MAX_CPUS 64u
#define AARCH64_GIC_MAX_TARGETS 8u
#define AARCH64_GIC_SCHEDULER_SGI 1u
#define AARCH64_GIC_TIMER_PPI 27u

#define AARCH64_GICD_CTLR 0x000u
#define AARCH64_GICD_IGROUPR0 0x080u
#define AARCH64_GICD_ISENABLER0 0x100u
#define AARCH64_GICD_ICENABLER0 0x180u
#define AARCH64_GICD_IPRIORITYR 0x400u
#define AARCH64_GICD_ITARGETSR 0x800u
#define AARCH64_GICD_SGIR 0xf00u

#define AARCH64_GICC_CTLR 0x000u
#define AARCH64_GICC_PMR 0x004u
#define AARCH64_GICC_IAR 0x00cu
#define AARCH64_GICC_EOIR 0x010u

#define AARCH64_GICD_CTLR_ENABLE_GRP1 (1u << 1)
#define AARCH64_GICC_CTLR_ENABLE_GRP1 (1u << 1)
#define AARCH64_GICC_IAR_INTID_MASK 0x3ffu
#define AARCH64_GICC_INTID_SPURIOUS_MIN 1020u

#define AARCH64_CNTV_CTL_ENABLE (1u << 0)
#define AARCH64_CNTV_CTL_ISTATUS (1u << 2)

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
static uint8_t             gic_target_masks[AARCH64_GIC_MAX_CPUS];
static bool                gic_scheduler_kick_pending[AARCH64_GIC_MAX_CPUS];
static struct spinlock     clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);

static inline uintptr_t phys_to_virt(uintptr_t phys) {
	struct kernel_boot_address_space address_space;

	if (!kernel_boot_address_space_get(&address_space)) return 0u;
	return address_space.direct_map_offset + phys;
}

static inline uint8_t mmio_read8(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint8_t*)(base + offset);
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

static inline uint32_t read_timer_control(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(value));
	return (uint32_t)value;
}

static bool map_mmio_page(uintptr_t phys) {
	uintptr_t page_phys = phys & ~(uintptr_t)(AARCH64_MMIO_PAGE_SIZE - 1u);
	uintptr_t page_virt;
	uintptr_t existing_phys = 0;

	page_virt = phys_to_virt(page_phys);
	if (page_virt == 0u) return false;
	if (hal_paging_query(hal_paging_kernel_space(), page_virt, &existing_phys, NULL)) return true;

	return hal_paging_map(
		hal_paging_kernel_space(), page_virt, page_phys, HAL_PAGE_WRITE | HAL_PAGE_GLOBAL | HAL_PAGE_NO_CACHE);
}

static bool gic_is_ready(void) {
	return __atomic_load_n(&gic_ready, __ATOMIC_ACQUIRE);
}

static bool gic_init_global(void) {
	if (gic_is_ready()) return true;

	if (!map_mmio_page(AARCH64_GICD_BASE_PHYS) || !map_mmio_page(AARCH64_GICC_BASE_PHYS)) {
		return false;
	}

	gicd_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(AARCH64_GICD_BASE_PHYS);
	gicc_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(AARCH64_GICC_BASE_PHYS);

	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, 0u);
	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, AARCH64_GICD_CTLR_ENABLE_GRP1);
	sync();

	__atomic_store_n(&gic_ready, true, __ATOMIC_RELEASE);
	return true;
}

static bool gic_target_mask_valid(uint8_t mask) {
	return mask != 0u && (mask & (uint8_t)(mask - 1u)) == 0u;
}

bool aarch64_gic_init_local(struct cpu* cpu) {
	uint32_t group;
	uint8_t  target_mask;

	if (cpu == NULL || cpu != cpu_current() || cpu->index >= AARCH64_GIC_MAX_CPUS) return false;
	if (!gic_is_ready()) return true;

	group = mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0);
	group |= 1u << AARCH64_GIC_SCHEDULER_SGI;
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0, group);
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + AARCH64_GIC_SCHEDULER_SGI, 0x40u);
	mmio_write32(gicd_mmio, AARCH64_GICD_ISENABLER0, 1u << AARCH64_GIC_SCHEDULER_SGI);
	mmio_write32(gicc_mmio, AARCH64_GICC_PMR, 0xffu);
	mmio_write32(gicc_mmio, AARCH64_GICC_CTLR, AARCH64_GICC_CTLR_ENABLE_GRP1);
	sync();

	/* ITARGETSR0-7 are banked and read-only for SGIs/PPIs on GICv2.
	 * Reading one SGI byte therefore gives this CPU interface's target bit. */
	target_mask = mmio_read8(gicd_mmio, AARCH64_GICD_ITARGETSR + AARCH64_GIC_SCHEDULER_SGI);
	if (!gic_target_mask_valid(target_mask)) {
		if (cpu_count() > 1u) return false;
		target_mask = 0u;
	}
	__atomic_store_n(&gic_target_masks[cpu->index], target_mask, __ATOMIC_RELEASE);
	return true;
}

bool aarch64_gic_prepare_smp(void) {
	if (cpu_count() > AARCH64_GIC_MAX_TARGETS) return false;
	if (!gic_init_global()) return false;
	return aarch64_gic_init_local(cpu_current());
}

bool aarch64_gic_send_scheduler_kick(const struct cpu* cpu) {
	bool    expected = false;
	uint8_t target_mask;

	if (cpu == NULL || cpu->index >= AARCH64_GIC_MAX_CPUS || !gic_is_ready()) return false;
	target_mask = __atomic_load_n(&gic_target_masks[cpu->index], __ATOMIC_ACQUIRE);
	if (!gic_target_mask_valid(target_mask)) return false;

	/* One outstanding SGI is enough: scheduler requests and remote ticks
	 * remain published until the target consumes them on interrupt exit. */
	if (!__atomic_compare_exchange_n(
			&gic_scheduler_kick_pending[cpu->index], &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return true;
	}

	/* Publish scheduler state before making the SGI visible to the target. */
	__asm__ volatile("dsb ishst" : : : "memory");
	mmio_write32(gicd_mmio, AARCH64_GICD_SGIR, ((uint32_t)target_mask << 16u) | AARCH64_GIC_SCHEDULER_SGI);
	return true;
}

static void gic_enable_timer_irq(void) {
	uint32_t group = mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0);

	group |= 1u << AARCH64_GIC_TIMER_PPI;
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0, group);
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + AARCH64_GIC_TIMER_PPI, 0x80u);
	mmio_write32(gicd_mmio, AARCH64_GICD_ICENABLER0, 1u << AARCH64_GIC_TIMER_PPI);
	mmio_write32(gicd_mmio, AARCH64_GICD_ISENABLER0, 1u << AARCH64_GIC_TIMER_PPI);
	sync();
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

static bool clock_fire(void) {
	if (!clock_running || clock_handler == NULL) return false;
	program_next_deadline();
	clock_handler(clock_context);
	return true;
}

static bool gic_handle_scheduler_kick(const struct exception_frame* frame) {
	struct cpu* cpu;
	uint32_t    iar;
	uint32_t    intid;

	if (frame == NULL || !is_irq_vector(frame->vector) || !gic_is_ready()) return false;
	cpu = cpu_current();
	if (cpu == NULL || cpu->index >= AARCH64_GIC_MAX_CPUS) return false;
	if (!__atomic_load_n(&gic_scheduler_kick_pending[cpu->index], __ATOMIC_ACQUIRE)) return false;

	iar   = mmio_read32(gicc_mmio, AARCH64_GICC_IAR);
	intid = iar & AARCH64_GICC_IAR_INTID_MASK;
	if (intid == AARCH64_GIC_SCHEDULER_SGI) {
		mmio_write32(gicc_mmio, AARCH64_GICC_EOIR, iar);
		__atomic_store_n(&gic_scheduler_kick_pending[cpu->index], false, __ATOMIC_RELEASE);
		return true;
	}

	/* A timer may win the race between publishing kick_pending and the SGIR
	 * write. If so, finish that IRQ and leave the scheduler SGI pending. */
	if (intid == AARCH64_GIC_TIMER_PPI) {
		bool handled = clock_fire();
		mmio_write32(gicc_mmio, AARCH64_GICC_EOIR, iar);
		return handled;
	}

	if (intid >= AARCH64_GICC_INTID_SPURIOUS_MIN) return false;
	mmio_write32(gicc_mmio, AARCH64_GICC_EOIR, iar);
	return false;
}

void hal_clock_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);

	if (clock_initialized) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return;
	}

	write_timer_control(0u);

	clock_initialized = true;
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

	if (clock_running) {
		write_timer_control(0u);
		clock_running        = false;
		clock_frequency_hz   = 0u;
		clock_interval_ticks = 0u;
		clock_next_deadline  = 0u;
		clock_handler        = NULL;
		clock_context        = NULL;
	}
	if (!gic_init_global() || !aarch64_gic_init_local(cpu_current())) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	gic_enable_timer_irq();

	counter_hz = read_counter_frequency();
	if (counter_hz == 0u) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}

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

	write_timer_control(0u);

	clock_running        = false;
	clock_frequency_hz   = 0u;
	clock_interval_ticks = 0u;
	clock_next_deadline  = 0u;
	clock_handler        = NULL;
	clock_context        = NULL;
	spinlock_unlock_irqrestore(&clock_lock, state);
}

bool clock_handle_irq(const struct exception_frame* frame) {
	if (frame == NULL || !is_irq_vector(frame->vector) || !gic_is_ready()) return false;
	if (gic_handle_scheduler_kick(frame)) return true;
	if ((read_timer_control() & AARCH64_CNTV_CTL_ISTATUS) == 0u) return false;

	/*
	 * The current QEMU/EDK2 AArch64 boot path delivers the timer IRQ once the
	 * PPI is unmasked through the GIC MMIO window, but the obvious acknowledge
	 * paths either trap at EL1 or report spurious IDs. Re-arming the timer is
	 * enough to drop the line on this setup, so keep that path as the fallback.
	 */
	return clock_fire();
}
