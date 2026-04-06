#include <hal/serial.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PL011_BASE_PHYS 0x09000000ull
#define PL011_BLOCK_SIZE 0x00200000ull

#define PL011_DR 0x000u
#define PL011_FR 0x018u
#define PL011_IBRD 0x024u
#define PL011_FBRD 0x028u
#define PL011_LCRH 0x02Cu
#define PL011_CR 0x030u
#define PL011_IMSC 0x038u
#define PL011_ICR 0x044u

#define PL011_FR_BUSY (1u << 3)
#define PL011_FR_TXFF (1u << 5)

#define PL011_LCRH_FEN (1u << 4)
#define PL011_LCRH_WLEN_8BIT (3u << 5)

#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE (1u << 8)
#define PL011_CR_RXE (1u << 9)

#define PL011_INT_ALL 0x7FFu

#define AARCH64_TT_DESC_VALID (1ull << 0)
#define AARCH64_TT_DESC_TABLE (1ull << 1)
#define AARCH64_TT_DESC_AF (1ull << 10)
#define AARCH64_TT_DESC_PXN (1ull << 53)
#define AARCH64_TT_DESC_UXN (1ull << 54)

#define AARCH64_TT_ATTR_DEVICE_nGnRnE 2ull

static volatile uint32_t* serial_base;
static bool               serial_ready;
static bool               serial_mapped;

static uint64_t ttbr0_l0[512] __attribute__((aligned(4096)));
static uint64_t ttbr0_l1[512] __attribute__((aligned(4096)));
static uint64_t ttbr0_l2[512] __attribute__((aligned(4096)));

static inline uint64_t kernel_virt_to_phys(const void* ptr) {
	struct kernel_boot_address_space address_space;

	if (!kernel_boot_address_space_get(&address_space)) return 0u;
	return address_space.physical_base + ((uint64_t)(uintptr_t)ptr - address_space.virtual_base);
}

static inline void serial_tlb_flush_all(void) {
	__asm__ volatile("dsb ishst\n\t"
	                 "tlbi vmalle1\n\t"
	                 "dsb ish\n\t"
	                 "isb" ::
	                     : "memory");
}

static bool serial_map_uart_identity(void) {
	if (serial_mapped) {
		return true;
	}

	uint64_t l0_phys = kernel_virt_to_phys(ttbr0_l0);
	uint64_t l1_phys = kernel_virt_to_phys(ttbr0_l1);
	uint64_t l2_phys = kernel_virt_to_phys(ttbr0_l2);

	if (l0_phys == 0 || l1_phys == 0 || l2_phys == 0) {
		return false;
	}

	ttbr0_l0[0] = l1_phys | AARCH64_TT_DESC_VALID | AARCH64_TT_DESC_TABLE;
	ttbr0_l1[0] = l2_phys | AARCH64_TT_DESC_VALID | AARCH64_TT_DESC_TABLE;

	ttbr0_l2[(PL011_BASE_PHYS >> 21) & 0x1FFu] = (PL011_BASE_PHYS & ~(PL011_BLOCK_SIZE - 1)) | AARCH64_TT_DESC_VALID |
	                                             ((uint64_t)AARCH64_TT_ATTR_DEVICE_nGnRnE << 2) | AARCH64_TT_DESC_AF |
	                                             AARCH64_TT_DESC_PXN | AARCH64_TT_DESC_UXN;

	__asm__ volatile("msr ttbr0_el1, %0\n\t"
	                 "isb"
	                 :
	                 : "r"(l0_phys)
	                 : "memory");
	serial_tlb_flush_all();

	serial_base   = (volatile uint32_t*)(uintptr_t)PL011_BASE_PHYS;
	serial_mapped = true;
	return true;
}

static inline volatile uint32_t* pl011_regs(void) {
	if (serial_base != NULL) {
		return serial_base;
	}

	if (!serial_map_uart_identity()) {
		return NULL;
	}

	return serial_base;
}

static inline uint32_t pl011_read(uint32_t offset) {
	volatile uint32_t* regs = pl011_regs();
	return regs[offset / sizeof(uint32_t)];
}

static inline void pl011_write(uint32_t offset, uint32_t value) {
	volatile uint32_t* regs         = pl011_regs();
	regs[offset / sizeof(uint32_t)] = value;
}

static inline void serial_relax(void) {
	__asm__ volatile("" ::: "memory");
}

static inline void pl011_wait_not_busy(void) {
	while ((pl011_read(PL011_FR) & PL011_FR_BUSY) != 0) {
		serial_relax();
	}
}

static inline void pl011_wait_tx_ready(void) {
	while ((pl011_read(PL011_FR) & PL011_FR_TXFF) != 0) {
		serial_relax();
	}
}

void hal_serial_init(void) {
	volatile uint32_t* regs = pl011_regs();
	if (serial_ready || regs == NULL) {
		return;
	}

	pl011_write(PL011_CR, 0u);
	pl011_wait_not_busy();

	pl011_write(PL011_IMSC, 0u);
	pl011_write(PL011_ICR, PL011_INT_ALL);

	/*
	 * QEMU's aarch64 virt machine exposes a PL011 clocked at 24 MHz.
	 * 24,000,000 / (16 * 115,200) = 13.0208..., so the closest divisor
	 * pair is IBRD=13 and FBRD=1.
	 */
	pl011_write(PL011_IBRD, 13u);
	pl011_write(PL011_FBRD, 1u);
	pl011_write(PL011_LCRH, PL011_LCRH_FEN | PL011_LCRH_WLEN_8BIT);
	pl011_write(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

	serial_ready = true;
}

void hal_serial_write_char(char ch) {
	if (!serial_ready) {
		hal_serial_init();
	}

	if (!serial_ready) {
		return;
	}

	pl011_wait_tx_ready();
	pl011_write(PL011_DR, (uint32_t)(uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}
