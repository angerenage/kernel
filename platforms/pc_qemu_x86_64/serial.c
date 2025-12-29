#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COM1_BASE 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static bool serial_ready;

static inline bool serial_transmit_empty(void) {
	return (inb(COM1_BASE + 5) & 0x20u) != 0;
}

void hal_serial_init(void) {
	if (serial_ready) {
		return;
	}

	outb(COM1_BASE + 1, 0x00); // Disable interrupts.
	outb(COM1_BASE + 3, 0x80); // Enable DLAB.
	outb(COM1_BASE + 0, 0x03); // Set divisor to 3 (38400 baud).
	outb(COM1_BASE + 1, 0x00); // High byte of divisor.
	outb(COM1_BASE + 3, 0x03); // 8 bits, no parity, one stop bit.
	outb(COM1_BASE + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold.
	outb(COM1_BASE + 4, 0x0B); // IRQs enabled, RTS/DSR set.

	serial_ready = true;
}

void hal_serial_write_char(char ch) {
	if (!serial_ready) {
		hal_serial_init();
	}

	if (!serial_ready) {
		return;
	}

	while (!serial_transmit_empty()) {
		__asm__ volatile("pause");
	}

	outb(COM1_BASE, (uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}
