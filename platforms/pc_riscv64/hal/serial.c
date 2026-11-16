#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <boot/info.h>
#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <hal/serial.h>

#define NS16550_PAGE_SIZE 0x1000ull

#define ACPI_SPCR_INTERFACE_TYPE_OFFSET 36u
#define ACPI_SPCR_GAS_OFFSET 40u
#define ACPI_SPCR_GAS_ADDRESS_OFFSET 44u
#define ACPI_SPCR_MIN_SIZE 52u
#define ACPI_GAS_SYSTEM_MEMORY 0u

#define NS16550_THR 0u
#define NS16550_LSR 5u

#define NS16550_LSR_THRE 0x20u
#define SERIAL_EMERGENCY_POLL_LIMIT 1000000u

#define RISCV_PTE_V (1ull << 0)
#define RISCV_PTE_R (1ull << 1)
#define RISCV_PTE_W (1ull << 2)
#define RISCV_PTE_X (1ull << 3)
#define RISCV_PTE_A (1ull << 6)
#define RISCV_PTE_D (1ull << 7)

#define RISCV_SATP_MODE_SV39 8ull
#define RISCV_SATP_MODE_SV48 9ull
#define RISCV_SATP_MODE_SV57 10ull

static volatile uint8_t* serial_base;
static uintptr_t serial_base_phys;
static struct dt_node serial_dt_node = DT_NODE_INVALID;
static bool serial_from_acpi;
static bool serial_ready;
static bool serial_emergency_active;
static bool serial_mapped;

static uint64_t serial_page_tables[4][512] __attribute__((aligned(4096)));
static size_t serial_page_table_count;

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
			reg.address > UINTPTR_MAX || reg.size < 8u)
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
	if (address == 0u || address > UINTPTR_MAX) return false;
	serial_from_acpi = true;
	*out_address = (uintptr_t)address;
	return true;
}

static bool serial_discover(void) {
	uintptr_t address;

	if (serial_base_phys != 0u) return true;
	if (!serial_find_dt(&address) && !serial_find_acpi(&address)) return false;
	serial_base_phys = address;
	return true;
}

size_t riscv64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	if (!serial_from_acpi) return 0u;
	if (tables != NULL && capacity != 0u) tables[0] = (struct hal_acpi_consumed_table){.signature = "SPCR"};
	return 1u;
}

size_t riscv64_serial_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	if (!dt_node_valid(serial_dt_node)) return 0u;
	if (nodes != NULL && capacity != 0u) {
		nodes[0] = (struct hal_device_tree_consumed_node){
			.node = serial_dt_node, .kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED, .value = 0u
		};
	}
	return 1u;
}

static inline uint64_t kernel_virt_to_phys(const void* ptr) {
	struct boot_address_space address_space;

	if (!boot_address_space_get(&address_space)) return 0u;
	return address_space.kernel_physical_base + ((uint64_t)(uintptr_t)ptr - address_space.kernel_virtual_base);
}

static inline uint64_t riscv_pte_from_phys(uint64_t phys) {
	return (phys >> 12) << 10;
}

static inline uint64_t riscv_pte_to_phys(uint64_t pte) {
	return (pte >> 10) << 12;
}

static inline uintptr_t hhdm_phys_to_virt(uint64_t phys) {
	struct boot_address_space address_space;

	if (!boot_address_space_get(&address_space)) return 0u;
	return address_space.direct_map_offset + phys;
}

static inline void riscv_tlb_flush_all(void) {
	__asm__ volatile("sfence.vma x0, x0" ::: "memory");
}

static bool serial_map_uart_hhdm(void) {
	if (serial_mapped) return true;
	if (!serial_discover()) return false;

	struct boot_address_space address_space;

	if (!boot_address_space_get(&address_space)) {
		return false;
	}

	uint64_t satp;
	__asm__ volatile("csrr %0, satp" : "=r"(satp));

	uint64_t mode = satp >> 60;
	int levels;
	switch (mode) {
	case RISCV_SATP_MODE_SV39:
		levels = 3;
		break;
	case RISCV_SATP_MODE_SV48:
		levels = 4;
		break;
	case RISCV_SATP_MODE_SV57:
		levels = 5;
		break;
	default:
		return false;
	}

	uint64_t root_phys = (satp & ((1ull << 44) - 1)) << 12;
	if (root_phys == 0) {
		return false;
	}

	if (serial_base_phys > UINTPTR_MAX - address_space.direct_map_offset) return false;
	if ((serial_base_phys & (NS16550_PAGE_SIZE - 1u)) > NS16550_PAGE_SIZE - 8u) return false;
	uint64_t uart_virt = address_space.direct_map_offset + serial_base_phys;
	uint64_t* table = (uint64_t*)(uintptr_t)hhdm_phys_to_virt(root_phys);
	if (table == NULL) {
		return false;
	}

	for (int level = levels - 1; level > 0; level--) {
		size_t index = (size_t)((uart_virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & RISCV_PTE_V) == 0) {
			if (serial_page_table_count >= (sizeof(serial_page_tables) / sizeof(serial_page_tables[0]))) {
				return false;
			}

			uint64_t* next_table = serial_page_tables[serial_page_table_count++];
			uint64_t next_phys = kernel_virt_to_phys(next_table);
			if (next_phys == 0) {
				return false;
			}

			table[index] = riscv_pte_from_phys(next_phys) | RISCV_PTE_V;
			entry = table[index];
		}
		else if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0) {
			return false;
		}

		table = (uint64_t*)(uintptr_t)hhdm_phys_to_virt(riscv_pte_to_phys(entry));
		if (table == NULL) {
			return false;
		}
	}

	size_t leaf_index = (size_t)((uart_virt >> 12) & 0x1ffu);
	table[leaf_index] = riscv_pte_from_phys(serial_base_phys & ~(NS16550_PAGE_SIZE - 1)) | RISCV_PTE_V | RISCV_PTE_R |
	                    RISCV_PTE_W | RISCV_PTE_A | RISCV_PTE_D;

	riscv_tlb_flush_all();

	serial_base = (volatile uint8_t*)(uintptr_t)uart_virt;
	serial_mapped = true;
	return true;
}

static inline volatile uint8_t* ns16550_regs(void) {
	if (serial_base != NULL) {
		return serial_base;
	}

	if (!serial_map_uart_hhdm()) {
		return NULL;
	}

	return serial_base;
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
	regs = serial_base;
	if (regs == NULL) return false;
	for (size_t i = 0u; i < length; i++) {
		if (data[i] == '\n' && !serial_emergency_write_char(regs, '\r')) return false;
		if (!serial_emergency_write_char(regs, data[i])) return false;
	}
	return true;
}
