#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS16550_BASE_PHYS 0x1fe001e0ull
#define LOONGARCH_DMW0_CSR 0x180u
#define LOONGARCH_DMW_PLV0 (1ull << 0)
#define LOONGARCH_DMW_VSEG 0x9000000000000000ull

#define NS16550_THR 0u
#define NS16550_IER 1u
#define NS16550_FCR 2u
#define NS16550_LCR 3u
#define NS16550_LSR 5u

#define NS16550_LCR_DLAB 0x80u
#define NS16550_LCR_8N1 0x03u
#define NS16550_FCR_FIFO 0xC7u

#define NS16550_LSR_THRE 0x20u

static bool serial_ready;

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
	serial_enable_uart_window();
	return (volatile uint8_t*)(uintptr_t)(LOONGARCH_DMW_VSEG | NS16550_BASE_PHYS);
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
