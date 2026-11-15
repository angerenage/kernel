#include "clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <hal/clock.h>
#include <hal/interrupts.h>
#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>

#include "interrupts/gic.h"

#define AARCH64_CNTV_CTL_ENABLE (1u << 0)
#define AARCH64_GIC_PPI_BASE 16u
#define AARCH64_GIC_PPI_COUNT 16u
#define AARCH64_DT_GIC_CELLS 3u
#define AARCH64_DT_VIRTUAL_TIMER_INDEX 2u
#define ACPI_GTDT_VIRTUAL_TIMER_GSIV_OFFSET 64u
#define ACPI_GTDT_VIRTUAL_TIMER_MIN_SIZE (ACPI_GTDT_VIRTUAL_TIMER_GSIV_OFFSET + sizeof(uint32_t))

static hal_clock_handler_t clock_handler;
static void* clock_context;
static bool clock_initialized;
static bool clock_running;
static uint32_t clock_frequency_hz;
static uint64_t clock_interval_ticks;
static uint64_t clock_next_deadline;
static uint64_t monotonic_frequency_hz;
static uint32_t timer_interrupt_id;
static struct dt_node timer_dt_node = DT_NODE_INVALID;
static bool timer_from_acpi;
static struct spinlock clock_lock = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
static struct hal_interrupt_source_state timer_source;

static bool clock_find_dt(uint32_t* out_id) {
	struct dt_node node = dt_device_at("arm,armv8-timer", 0u);
	struct dt_property interrupts;
	uint64_t type;
	uint64_t number;
	size_t first_cell = AARCH64_DT_VIRTUAL_TIMER_INDEX * AARCH64_DT_GIC_CELLS;

	if (!dt_node_valid(node)) node = dt_device_at("arm,armv7-timer", 0u);
	if (!dt_node_valid(node) || !dt_node_property(node, "interrupts", &interrupts) ||
			interrupts.size < (first_cell + AARCH64_DT_GIC_CELLS) * sizeof(uint32_t) ||
			interrupts.size % (AARCH64_DT_GIC_CELLS * sizeof(uint32_t)) != 0u ||
			!dt_property_read_cells(&interrupts, first_cell, 1u, &type) || type != 1u ||
			!dt_property_read_cells(&interrupts, first_cell + 1u, 1u, &number) || number >= AARCH64_GIC_PPI_COUNT)
		return false;
	timer_dt_node = node;
	*out_id = AARCH64_GIC_PPI_BASE + (uint32_t)number;
	return true;
}

static bool clock_find_acpi(uint32_t* out_id) {
	const struct acpi_sdt_header* gtdt = acpi_table_next("GTDT", NULL);
	uint32_t id;

	if (gtdt == NULL || gtdt->length < ACPI_GTDT_VIRTUAL_TIMER_MIN_SIZE) return false;
	memcpy(&id, (const uint8_t*)gtdt + ACPI_GTDT_VIRTUAL_TIMER_GSIV_OFFSET, sizeof(id));
	if (id < AARCH64_GIC_PPI_BASE || id >= AARCH64_GIC_PPI_BASE + AARCH64_GIC_PPI_COUNT) return false;
	timer_from_acpi = true;
	*out_id = id;
	return true;
}

static bool clock_discover_interrupt(void) {
	uint32_t id;

	if (timer_interrupt_id != 0u) return true;
	if (!clock_find_dt(&id) && !clock_find_acpi(&id)) return false;
	timer_interrupt_id = id;
	return true;
}

size_t aarch64_clock_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	(void)clock_discover_interrupt();
	if (!timer_from_acpi) return 0u;
	if (tables != NULL && capacity != 0u) tables[0] = (struct hal_acpi_consumed_table){.signature = "GTDT"};
	return 1u;
}

size_t aarch64_clock_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	(void)clock_discover_interrupt();
	if (!dt_node_valid(timer_dt_node)) return 0u;
	if (nodes != NULL && capacity != 0u) {
		nodes[0] = (struct hal_device_tree_consumed_node){
			.node = timer_dt_node,
			.kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED,
			.value = 0u,
		};
	}
	return 1u;
}

bool aarch64_clock_is_interrupt(uint32_t id) {
	return timer_interrupt_id != 0u && id == timer_interrupt_id;
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
		(void)clock_discover_interrupt();
		monotonic_frequency_hz = read_counter_frequency();
		clock_initialized = true;
	}
	spinlock_unlock_irqrestore(&clock_lock, state);
}

uint64_t hal_clock_monotonic_frequency(void) {
	return __atomic_load_n(&monotonic_frequency_hz, __ATOMIC_ACQUIRE);
}

bool hal_clock_monotonic_counter(uint64_t* out_counter) {
	if (out_counter == NULL || hal_clock_monotonic_frequency() == 0u) return false;
	*out_counter = read_counter();
	return true;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	uint64_t counter_hz;
	uint64_t interval_ticks;
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
	counter_hz = read_counter_frequency();
	if (counter_hz == 0u) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	interval_ticks = counter_hz / frequency_hz;
	if (interval_ticks == 0u) interval_ticks = 1u;
	if (timer_interrupt_id == 0u || !aarch64_gic_local_source_init(&timer_source, timer_interrupt_id, cpu_current())) {
		spinlock_unlock_irqrestore(&clock_lock, state);
		return false;
	}
	clock_handler = handler;
	clock_context = ctx;
	clock_interval_ticks = interval_ticks;
	clock_next_deadline = read_counter() + interval_ticks;
	clock_frequency_hz = (uint32_t)(counter_hz / interval_ticks);
	clock_running = true;
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
	uint32_t hz = clock_frequency_hz;
	spinlock_unlock_irqrestore(&clock_lock, state);
	return hz;
}

void hal_clock_stop(void) {
	struct irq_state state = spinlock_lock_irqsave(&clock_lock);
	if (clock_initialized) {
		if (aarch64_gic_local_source_deinit(&timer_source)) {
			write_timer_control(0u);
			clock_running = false;
			clock_frequency_hz = 0u;
			clock_interval_ticks = 0u;
			clock_next_deadline = 0u;
			clock_handler = NULL;
			clock_context = NULL;
		}
	}
	spinlock_unlock_irqrestore(&clock_lock, state);
}
