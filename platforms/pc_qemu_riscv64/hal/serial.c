#include <hal/serial.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS16550_BASE_PHYS 0x10000000ull
#define NS16550_PAGE_SIZE 0x1000ull

#define NS16550_THR 0u
#define NS16550_IER 1u
#define NS16550_FCR 2u
#define NS16550_LCR 3u
#define NS16550_LSR 5u

#define NS16550_LCR_DLAB 0x80u
#define NS16550_LCR_8N1 0x03u
#define NS16550_FCR_FIFO 0xC7u

#define NS16550_LSR_THRE 0x20u

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
static bool              serial_ready;
static bool              serial_mapped;

static uint64_t serial_page_tables[4][512] __attribute__((aligned(4096)));
static size_t   serial_page_table_count;

static inline uint64_t kernel_virt_to_phys(const void* ptr) {
	if (exec_addr_req.response == NULL) {
		return 0;
	}

	return exec_addr_req.response->physical_base + ((uint64_t)(uintptr_t)ptr - exec_addr_req.response->virtual_base);
}

static inline uint64_t riscv_pte_from_phys(uint64_t phys) {
	return (phys >> 12) << 10;
}

static inline uint64_t riscv_pte_to_phys(uint64_t pte) {
	return (pte >> 10) << 12;
}

static inline uintptr_t hhdm_phys_to_virt(uint64_t phys) {
	if (hhdm_req.response == NULL) {
		return 0;
	}

	return (uintptr_t)(hhdm_req.response->offset + phys);
}

static inline void riscv_tlb_flush_all(void) {
	__asm__ volatile("sfence.vma x0, x0" ::: "memory");
}

static bool serial_map_uart_hhdm(void) {
	if (serial_mapped) {
		return true;
	}

	if (hhdm_req.response == NULL || exec_addr_req.response == NULL) {
		return false;
	}

	uint64_t satp;
	__asm__ volatile("csrr %0, satp" : "=r"(satp));

	uint64_t mode = satp >> 60;
	int      levels;
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

	uint64_t  uart_virt = hhdm_req.response->offset + NS16550_BASE_PHYS;
	uint64_t* table     = (uint64_t*)(uintptr_t)hhdm_phys_to_virt(root_phys);
	if (table == NULL) {
		return false;
	}

	for (int level = levels - 1; level > 0; level--) {
		size_t   index = (size_t)((uart_virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & RISCV_PTE_V) == 0) {
			if (serial_page_table_count >= (sizeof(serial_page_tables) / sizeof(serial_page_tables[0]))) {
				return false;
			}

			uint64_t* next_table = serial_page_tables[serial_page_table_count++];
			uint64_t  next_phys  = kernel_virt_to_phys(next_table);
			if (next_phys == 0) {
				return false;
			}

			table[index] = riscv_pte_from_phys(next_phys) | RISCV_PTE_V;
			entry        = table[index];
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
	table[leaf_index] = riscv_pte_from_phys(NS16550_BASE_PHYS & ~(NS16550_PAGE_SIZE - 1)) | RISCV_PTE_V | RISCV_PTE_R |
	                    RISCV_PTE_W | RISCV_PTE_A | RISCV_PTE_D;

	riscv_tlb_flush_all();

	serial_base   = (volatile uint8_t*)(uintptr_t)uart_virt;
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
	regs[reg]              = value;
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
	if (serial_ready || regs == NULL) {
		return;
	}

	ns16550_write(NS16550_IER, 0x00u);
	ns16550_write(NS16550_LCR, NS16550_LCR_DLAB);
	ns16550_write(NS16550_THR, 0x01u);
	ns16550_write(NS16550_IER, 0x00u);
	ns16550_write(NS16550_LCR, NS16550_LCR_8N1);
	ns16550_write(NS16550_FCR, NS16550_FCR_FIFO);

	serial_ready = true;
}

void hal_serial_write_char(char ch) {
	if (!serial_ready) {
		hal_serial_init();
	}

	if (!serial_ready) {
		return;
	}

	ns16550_wait_tx_ready();
	ns16550_write(NS16550_THR, (uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}
