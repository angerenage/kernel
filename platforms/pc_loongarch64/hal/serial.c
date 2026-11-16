#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <hal/serial.h>

#define LOONGARCH_DMW0_CSR 0x180u
#define LOONGARCH_DMW_PLV0 (1ull << 0)
#define LOONGARCH_DMW_VSEG 0x9000000000000000ull
#define LOONGARCH_DMW_PHYS_LIMIT (1ull << 60u)

#define ACPI_SPCR_INTERFACE_TYPE_OFFSET 36u
#define ACPI_SPCR_GAS_OFFSET 40u
#define ACPI_SPCR_GAS_ADDRESS_OFFSET 44u
#define ACPI_SPCR_MIN_SIZE 52u
#define ACPI_GAS_SYSTEM_MEMORY 0u

#define NS16550_THR 0u
#define NS16550_LSR 5u

#define NS16550_LSR_THRE 0x20u
#define SERIAL_EMERGENCY_POLL_LIMIT 1000000u

static bool serial_ready;
static bool serial_emergency_active;
uintptr_t loongarch64_serial_base_physical;
static struct dt_node serial_dt_node = DT_NODE_INVALID;
static bool serial_from_acpi;

static bool serial_ns16550_node(struct dt_node node) {
	return dt_node_compatible(node, "ns16550a") || dt_node_compatible(node, "ns16550");
}

static bool serial_dt_default_register_layout(struct dt_node node) {
	struct dt_property property;
	uint64_t value;

	if (dt_node_property(node, "reg-shift", &property) &&
			(property.size != sizeof(uint32_t) || !dt_property_read_cells(&property, 0u, 1u, &value) || value != 0u))
		return false;
	if (dt_node_property(node, "reg-io-width", &property) &&
			(property.size != sizeof(uint32_t) || !dt_property_read_cells(&property, 0u, 1u, &value) || value != 1u))
		return false;
	return true;
}

static bool serial_find_dt(uintptr_t* out_address) {
	struct dt_node node = dt_stdout_node();
	struct dt_reg reg;

	if (!dt_node_valid(node)) node = dt_device_at("ns16550a", 0u);
	if (!dt_node_valid(node)) node = dt_device_at("ns16550", 0u);
	if (!dt_node_valid(node) || !dt_node_enabled(node) || !serial_ns16550_node(node) ||
			!serial_dt_default_register_layout(node) || !dt_node_reg(node, 0u, &reg) || reg.address == 0u ||
			reg.address >= LOONGARCH_DMW_PHYS_LIMIT || reg.size < 8u)
		return false;
	serial_dt_node = node;
	*out_address = (uintptr_t)reg.address;
	return true;
}

static bool serial_find_acpi(uintptr_t* out_address) {
	const struct acpi_sdt_header* spcr = acpi_table_next("SPCR", NULL);
	const uint8_t* bytes = (const uint8_t*)spcr;
	uint64_t address;

	if (spcr == NULL || spcr->length < ACPI_SPCR_MIN_SIZE || bytes[ACPI_SPCR_INTERFACE_TYPE_OFFSET] > 2u ||
			bytes[ACPI_SPCR_GAS_OFFSET] != ACPI_GAS_SYSTEM_MEMORY || bytes[ACPI_SPCR_GAS_OFFSET + 2u] != 0u)
		return false;
	memcpy(&address, bytes + ACPI_SPCR_GAS_ADDRESS_OFFSET, sizeof(address));
	if (address == 0u || address >= LOONGARCH_DMW_PHYS_LIMIT) return false;
	serial_from_acpi = true;
	*out_address = (uintptr_t)address;
	return true;
}

static bool serial_discover(void) {
	uintptr_t address;

	if (loongarch64_serial_base_physical != 0u) return true;
	if (!serial_find_dt(&address) && !serial_find_acpi(&address)) return false;
	loongarch64_serial_base_physical = address;
	return true;
}

size_t loongarch64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	if (!serial_from_acpi) return 0u;
	if (tables != NULL && capacity != 0u) tables[0] = (struct hal_acpi_consumed_table){.signature = "SPCR"};
	return 1u;
}

size_t loongarch64_serial_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	if (!dt_node_valid(serial_dt_node)) return 0u;
	if (nodes != NULL && capacity != 0u) {
		nodes[0] = (struct hal_device_tree_consumed_node){
			.node = serial_dt_node, .kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED, .value = 0u
		};
	}
	return 1u;
}

static inline void serial_enable_uart_window(void) {
	uint64_t current;
	uint64_t window = LOONGARCH_DMW_VSEG | LOONGARCH_DMW_PLV0;

	/* DMW CSRs are CPU-local, unlike the shared UART initialization state. */
	__asm__ volatile("csrrd %0, %1" : "=r"(current) : "i"(LOONGARCH_DMW0_CSR));
	if (current == window) return;
	__asm__ volatile("csrwr %0, %1" : : "r"(window), "i"(LOONGARCH_DMW0_CSR) : "memory");
	__asm__ volatile("dbar 0" ::: "memory");
}

static inline volatile uint8_t* ns16550_regs(void) {
	if (!serial_discover()) return NULL;
	serial_enable_uart_window();
	return (volatile uint8_t*)(uintptr_t)(LOONGARCH_DMW_VSEG | loongarch64_serial_base_physical);
}

static inline uint8_t ns16550_read(uint32_t reg) {
	volatile uint8_t* regs = ns16550_regs();
	return regs[reg];
}

static inline void ns16550_write(uint32_t reg, uint8_t value) {
	volatile uint8_t* regs = ns16550_regs();
	regs[reg] = value;
}

static inline void serial_relax(void) {
	__asm__ volatile("" ::: "memory");
}

static inline void ns16550_wait_tx_ready(void) {
	while ((ns16550_read(NS16550_LSR) & NS16550_LSR_THRE) == 0) {
		serial_relax();
	}
}

void hal_serial_init(void) {
	volatile uint8_t* regs = ns16550_regs();
	if (__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE) || regs == NULL) {
		return;
	}

	/* Preserve the firmware-selected baud rate and line settings. */
	__atomic_store_n(&serial_ready, true, __ATOMIC_RELEASE);
}

void hal_serial_write_char(char ch) {
	if (__atomic_load_n(&serial_emergency_active, __ATOMIC_ACQUIRE)) return;
	if (!__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE)) {
		hal_serial_init();
	}

	if (!__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE)) {
		return;
	}

	ns16550_wait_tx_ready();
	if (__atomic_load_n(&serial_emergency_active, __ATOMIC_ACQUIRE)) return;
	ns16550_write(NS16550_THR, (uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}

static bool serial_emergency_write_char(volatile uint8_t* regs, char ch) {
	for (size_t attempt = 0u; attempt < SERIAL_EMERGENCY_POLL_LIMIT; attempt++) {
		if ((regs[NS16550_LSR] & NS16550_LSR_THRE) != 0u) {
			regs[NS16550_THR] = (uint8_t)ch;
			return true;
		}
		serial_relax();
	}
	return false;
}

bool hal_serial_write_emergency(const char* data, size_t length) {
	volatile uint8_t* regs;

	__atomic_store_n(&serial_emergency_active, true, __ATOMIC_RELEASE);
	if (data == NULL || !__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE)) return false;
	serial_enable_uart_window();
	regs = (volatile uint8_t*)(uintptr_t)(LOONGARCH_DMW_VSEG | loongarch64_serial_base_physical);
	for (size_t i = 0u; i < length; i++) {
		if (data[i] == '\n' && !serial_emergency_write_char(regs, '\r')) return false;
		if (!serial_emergency_write_char(regs, data[i])) return false;
	}
	return true;
}
