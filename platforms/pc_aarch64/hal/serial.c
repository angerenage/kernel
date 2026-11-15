#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <boot/info.h>
#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <hal/paging.h>
#include <hal/serial.h>

#define PL011_PAGE_SIZE 0x00001000ull
#define PL011_BLOCK_SIZE 0x00200000ull
#define AARCH64_LOW_ADDRESS_LIMIT (1ull << 48u)

#define ACPI_SPCR_INTERFACE_TYPE_OFFSET 36u
#define ACPI_SPCR_GAS_OFFSET 40u
#define ACPI_SPCR_GAS_ADDRESS_OFFSET (ACPI_SPCR_GAS_OFFSET + 4u)
#define ACPI_SPCR_MIN_SIZE (ACPI_SPCR_GAS_ADDRESS_OFFSET + sizeof(uint64_t))
#define ACPI_SPCR_INTERFACE_ARM_PL011 0x03u
#define ACPI_GAS_SYSTEM_MEMORY 0x00u

#define PL011_DR 0x000u
#define PL011_FR 0x018u
#define PL011_CR 0x030u

#define PL011_FR_TXFF (1u << 5)

#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE (1u << 8)
#define SERIAL_EMERGENCY_POLL_LIMIT 1000000u

#define AARCH64_TT_DESC_VALID (1ull << 0)
#define AARCH64_TT_DESC_TABLE (1ull << 1)
#define AARCH64_TT_DESC_AF (1ull << 10)
#define AARCH64_TT_DESC_PXN (1ull << 53)
#define AARCH64_TT_DESC_UXN (1ull << 54)

#define AARCH64_TT_ATTR_DEVICE_nGnRnE 2ull

static volatile uint32_t* serial_base;
static uintptr_t serial_base_phys;
static struct dt_node serial_dt_node = DT_NODE_INVALID;
static bool serial_from_acpi;
static bool serial_ready;
static bool serial_emergency_active;
static bool serial_identity_mapped;
static bool serial_direct_mapped;

static uint64_t ttbr0_l0[512] __attribute__((aligned(4096)));
static uint64_t ttbr0_l1[512] __attribute__((aligned(4096)));
static uint64_t ttbr0_l2[512] __attribute__((aligned(4096)));

static inline uint64_t kernel_virt_to_phys(const void* ptr) {
	struct boot_address_space address_space;

	if (!boot_address_space_get(&address_space)) return 0u;
	return address_space.kernel_physical_base + ((uint64_t)(uintptr_t)ptr - address_space.kernel_virtual_base);
}

static inline void serial_tlb_flush_all(void) {
	__asm__ volatile("dsb ishst\n\t"
					 "tlbi vmalle1\n\t"
					 "dsb ish\n\t"
					 "isb" ::
							 : "memory");
}

static struct dt_node serial_dt_child_named(struct dt_node parent, const char* name, size_t name_length) {
	for (struct dt_node child = dt_node_child(parent); dt_node_valid(child); child = dt_node_next(child)) {
		const char* child_name = dt_node_name(child);
		if (child_name != NULL && strlen(child_name) == name_length && memcmp(child_name, name, name_length) == 0)
			return child;
	}
	return DT_NODE_INVALID;
}

static struct dt_node serial_dt_path(const char* path, size_t length) {
	struct dt_node node = dt_root();
	size_t cursor = 0u;

	if (!dt_node_valid(node) || path == NULL || length == 0u || path[0] != '/') return DT_NODE_INVALID;
	while (cursor < length && path[cursor] == '/') cursor++;
	while (cursor < length) {
		size_t start = cursor;
		while (cursor < length && path[cursor] != '/') cursor++;
		if (cursor == start || !dt_node_valid(node = serial_dt_child_named(node, path + start, cursor - start)))
			return DT_NODE_INVALID;
		while (cursor < length && path[cursor] == '/') cursor++;
	}
	return node;
}

static bool serial_dt_property_string(
		struct dt_node node, const char* name, const char** out_string, size_t* out_length) {
	struct dt_property property;
	const char* end;

	if (out_string == NULL || out_length == NULL || !dt_node_property(node, name, &property) || property.size == 0u)
		return false;
	end = memchr(property.data, '\0', property.size);
	if (end == NULL) return false;
	*out_string = property.data;
	*out_length = (size_t)(end - (const char*)property.data);
	return true;
}

static struct dt_node serial_dt_console(void) {
	struct dt_node root = dt_root();
	struct dt_node chosen;
	const char* path;
	size_t length;

	if (!dt_node_valid(root)) return DT_NODE_INVALID;
	chosen = serial_dt_child_named(root, "chosen", sizeof("chosen") - 1u);
	if (!dt_node_valid(chosen) || (!serial_dt_property_string(chosen, "stdout-path", &path, &length) &&
										  !serial_dt_property_string(chosen, "linux,stdout-path", &path, &length)))
		return DT_NODE_INVALID;
	for (size_t index = 0u; index < length; index++) {
		if (path[index] == ':') {
			length = index;
			break;
		}
	}
	if (length == 0u) return DT_NODE_INVALID;
	if (path[0] == '/') return serial_dt_path(path, length);

	struct dt_node aliases = serial_dt_child_named(root, "aliases", sizeof("aliases") - 1u);
	const char* alias_path;
	size_t alias_length;
	char alias[64];
	if (!dt_node_valid(aliases) || length >= sizeof(alias)) return DT_NODE_INVALID;
	memcpy(alias, path, length);
	alias[length] = '\0';
	if (!serial_dt_property_string(aliases, alias, &alias_path, &alias_length)) return DT_NODE_INVALID;
	return serial_dt_path(alias_path, alias_length);
}

static bool serial_find_dt(uintptr_t* out_address) {
	struct dt_node node = serial_dt_console();
	struct dt_reg reg;

	if (!dt_node_valid(node)) node = dt_device_at("arm,pl011", 0u);
	if (!dt_node_valid(node) || !dt_node_enabled(node) || !dt_node_compatible(node, "arm,pl011") ||
			!dt_node_reg(node, 0u, &reg) || reg.address == 0u || reg.address > UINTPTR_MAX ||
			reg.size < PL011_PAGE_SIZE)
		return false;
	serial_dt_node = node;
	*out_address = (uintptr_t)reg.address;
	return true;
}

static bool serial_find_acpi(uintptr_t* out_address) {
	const struct acpi_sdt_header* spcr = acpi_table_next("SPCR", NULL);
	const uint8_t* bytes = (const uint8_t*)spcr;
	uint64_t address;

	if (spcr == NULL || spcr->length < ACPI_SPCR_MIN_SIZE ||
			bytes[ACPI_SPCR_INTERFACE_TYPE_OFFSET] != ACPI_SPCR_INTERFACE_ARM_PL011 ||
			bytes[ACPI_SPCR_GAS_OFFSET] != ACPI_GAS_SYSTEM_MEMORY)
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

size_t aarch64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	if (!serial_from_acpi) return 0u;
	if (tables != NULL && capacity != 0u) tables[0] = (struct hal_acpi_consumed_table){.signature = "SPCR"};
	return 1u;
}

size_t aarch64_serial_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	if (!dt_node_valid(serial_dt_node)) return 0u;
	if (nodes != NULL && capacity != 0u) {
		nodes[0] = (struct hal_device_tree_consumed_node){
			.node = serial_dt_node,
			.kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED,
			.value = 0u,
		};
	}
	return 1u;
}

static bool serial_map_uart_identity(void) {
	uintptr_t block_phys;
	size_t l0_index;
	size_t l1_index;
	size_t l2_index;

	if (!serial_discover() || serial_base_phys >= AARCH64_LOW_ADDRESS_LIMIT) return false;
	if (serial_identity_mapped) {
		serial_base = (volatile uint32_t*)serial_base_phys;
		return true;
	}

	uint64_t l0_phys = kernel_virt_to_phys(ttbr0_l0);
	uint64_t l1_phys = kernel_virt_to_phys(ttbr0_l1);
	uint64_t l2_phys = kernel_virt_to_phys(ttbr0_l2);

	if (l0_phys == 0 || l1_phys == 0 || l2_phys == 0) {
		return false;
	}

	block_phys = serial_base_phys & ~(uintptr_t)(PL011_BLOCK_SIZE - 1u);
	l0_index = (serial_base_phys >> 39u) & 0x1ffu;
	l1_index = (serial_base_phys >> 30u) & 0x1ffu;
	l2_index = (serial_base_phys >> 21u) & 0x1ffu;
	ttbr0_l0[l0_index] = l1_phys | AARCH64_TT_DESC_VALID | AARCH64_TT_DESC_TABLE;
	ttbr0_l1[l1_index] = l2_phys | AARCH64_TT_DESC_VALID | AARCH64_TT_DESC_TABLE;
	ttbr0_l2[l2_index] = block_phys | AARCH64_TT_DESC_VALID | ((uint64_t)AARCH64_TT_ATTR_DEVICE_nGnRnE << 2) |
	                     AARCH64_TT_DESC_AF | AARCH64_TT_DESC_PXN | AARCH64_TT_DESC_UXN;

	__asm__ volatile("msr ttbr0_el1, %0\n\t"
					 "isb"
			:
			: "r"(l0_phys)
			: "memory");
	serial_tlb_flush_all();

	serial_base = (volatile uint32_t*)serial_base_phys;
	serial_identity_mapped = true;
	return true;
}

static bool serial_map_uart_direct(void) {
	struct boot_address_space address_space;
	struct hal_paging_space* kernel_space;
	uintptr_t page_phys;
	uintptr_t page_virt;

	if (serial_direct_mapped) return true;
	if (!serial_discover()) return false;
	if (!boot_address_space_get(&address_space)) return false;

	kernel_space = hal_paging_kernel_space();
	if (kernel_space == NULL) return false;

	page_phys = serial_base_phys & ~(uintptr_t)(PL011_PAGE_SIZE - 1u);
	if (page_phys > UINTPTR_MAX - address_space.direct_map_offset ||
			serial_base_phys > UINTPTR_MAX - address_space.direct_map_offset)
		return false;
	page_virt = (uintptr_t)(address_space.direct_map_offset + page_phys);
	if (!hal_paging_query(kernel_space, page_virt, NULL) &&
			!hal_paging_map(kernel_space,
					&(const struct hal_paging_map_request){
						page_virt,
						page_phys,
						PL011_PAGE_SIZE,
						HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL,
						MEMORY_TYPE_DEVICE
					})) {
		return false;
	}

	serial_base = (volatile uint32_t*)(address_space.direct_map_offset + serial_base_phys);
	serial_direct_mapped = true;
	return true;
}

static inline volatile uint32_t* pl011_regs(void) {
	if (serial_direct_mapped || serial_map_uart_direct()) {
		return serial_base;
	}

	if (serial_base != NULL) {
		return serial_base;
	}

	return serial_map_uart_identity() ? serial_base : NULL;
}

static inline uint32_t pl011_read(uint32_t offset) {
	volatile uint32_t* regs = pl011_regs();
	return regs[offset / sizeof(uint32_t)];
}

static inline void pl011_write(uint32_t offset, uint32_t value) {
	volatile uint32_t* regs = pl011_regs();
	regs[offset / sizeof(uint32_t)] = value;
}

static inline void serial_relax(void) {
	__asm__ volatile("" ::: "memory");
}

static inline void pl011_wait_tx_ready(void) {
	while ((pl011_read(PL011_FR) & PL011_FR_TXFF) != 0) {
		serial_relax();
	}
}

void hal_serial_init(void) {
	volatile uint32_t* regs = pl011_regs();
	if (__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE) || regs == NULL) {
		return;
	}

	pl011_write(PL011_CR, pl011_read(PL011_CR) | PL011_CR_UARTEN | PL011_CR_TXE);

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

	pl011_wait_tx_ready();
	if (__atomic_load_n(&serial_emergency_active, __ATOMIC_ACQUIRE)) return;
	pl011_write(PL011_DR, (uint32_t)(uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}

static bool serial_emergency_write_char(volatile uint32_t* regs, char ch) {
	for (size_t attempt = 0u; attempt < SERIAL_EMERGENCY_POLL_LIMIT; attempt++) {
		if ((regs[PL011_FR / sizeof(uint32_t)] & PL011_FR_TXFF) == 0u) {
			regs[PL011_DR / sizeof(uint32_t)] = (uint32_t)(uint8_t)ch;
			return true;
		}
		serial_relax();
	}
	return false;
}

bool hal_serial_write_emergency(const char* data, size_t length) {
	volatile uint32_t* regs;

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
