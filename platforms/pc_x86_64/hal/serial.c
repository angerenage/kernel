#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils.h"

#define COM1_BASE 0x3F8
#define SERIAL_EMERGENCY_POLL_LIMIT 1000000u

static bool serial_ready;
static bool serial_emergency_active;

static inline bool serial_transmit_empty(void) {
	return (inb(COM1_BASE + 5) & 0x20u) != 0;
}

void hal_serial_init(void) {
	if (__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE)) {
		return;
	}

	outb(COM1_BASE + 1, 0x00); // Disable interrupts.
	outb(COM1_BASE + 3, 0x80); // Enable DLAB.
	outb(COM1_BASE + 0, 0x03); // Set divisor to 3 (38400 baud).
	outb(COM1_BASE + 1, 0x00); // High byte of divisor.
	outb(COM1_BASE + 3, 0x03); // 8 bits, no parity, one stop bit.
	outb(COM1_BASE + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold.
	outb(COM1_BASE + 4, 0x0B); // IRQs enabled, RTS/DSR set.

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

	while (!serial_transmit_empty()) {
		__asm__ volatile("pause");
	}
	if (__atomic_load_n(&serial_emergency_active, __ATOMIC_ACQUIRE)) return;

	outb(COM1_BASE, (uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}

static bool serial_emergency_write_char(char ch) {
	for (size_t attempt = 0u; attempt < SERIAL_EMERGENCY_POLL_LIMIT; attempt++) {
		if (serial_transmit_empty()) {
			outb(COM1_BASE, (uint8_t)ch);
			return true;
		}
		__asm__ volatile("pause");
	}
	return false;
}

bool hal_serial_write_emergency(const char* data, size_t length) {
	__atomic_store_n(&serial_emergency_active, true, __ATOMIC_RELEASE);
	if (data == NULL || !__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE)) return false;
	for (size_t i = 0u; i < length; i++) {
		if (data[i] == '\n' && !serial_emergency_write_char('\r')) return false;
		if (!serial_emergency_write_char(data[i])) return false;
	}
	return true;
}
