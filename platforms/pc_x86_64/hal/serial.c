#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <firmware/acpi.h>
#include <hal/serial.h>

#include "utils.h"

#define LEGACY_COM1_BASE 0x3f8u
#define SERIAL_EMERGENCY_POLL_LIMIT 1000000u

#define ACPI_SPCR_INTERFACE_TYPE_OFFSET 36u
#define ACPI_SPCR_GAS_OFFSET 40u
#define ACPI_SPCR_GAS_ADDRESS_OFFSET 44u
#define ACPI_SPCR_MIN_SIZE 52u
#define ACPI_GAS_SYSTEM_IO 1u

static bool serial_ready;
static bool serial_emergency_active;
static uint16_t serial_port;
static bool serial_from_acpi;

static bool serial_find_acpi(uint16_t* out_port) {
	const struct acpi_sdt_header* spcr = acpi_table_next("SPCR", NULL);
	const uint8_t* bytes = (const uint8_t*)spcr;
	uint64_t address;

	if (spcr == NULL || spcr->length < ACPI_SPCR_MIN_SIZE || bytes[ACPI_SPCR_INTERFACE_TYPE_OFFSET] > 2u ||
			bytes[ACPI_SPCR_GAS_OFFSET] != ACPI_GAS_SYSTEM_IO || bytes[ACPI_SPCR_GAS_OFFSET + 2u] != 0u)
		return false;
	memcpy(&address, bytes + ACPI_SPCR_GAS_ADDRESS_OFFSET, sizeof(address));
	if (address == 0u || address > UINT16_MAX - 7u) return false;
	serial_from_acpi = true;
	*out_port = (uint16_t)address;
	return true;
}

static bool serial_discover(void) {
	if (serial_port != 0u) return true;
	if (!serial_find_acpi(&serial_port)) serial_port = LEGACY_COM1_BASE;
	return true;
}

size_t x86_64_serial_acpi_consumed_tables(struct hal_acpi_consumed_table* tables, size_t capacity) {
	if (!serial_from_acpi) return 0u;
	if (tables != NULL && capacity != 0u) tables[0] = (struct hal_acpi_consumed_table){.signature = "SPCR"};
	return 1u;
}

static inline bool serial_transmit_empty(void) {
	return (inb((uint16_t)(serial_port + 5u)) & 0x20u) != 0;
}

void hal_serial_init(void) {
	if (__atomic_load_n(&serial_ready, __ATOMIC_ACQUIRE) || !serial_discover()) return;

	if (!serial_from_acpi) {
		outb((uint16_t)(serial_port + 1u), 0x00); // Disable interrupts.
		outb((uint16_t)(serial_port + 3u), 0x80); // Enable DLAB.
		outb(serial_port, 0x03);                  // Set divisor to 3 (38400 baud).
		outb((uint16_t)(serial_port + 1u), 0x00); // High byte of divisor.
		outb((uint16_t)(serial_port + 3u), 0x03); // 8 bits, no parity, one stop bit.
		outb((uint16_t)(serial_port + 2u), 0xc7); // Enable and clear FIFOs.
		outb((uint16_t)(serial_port + 4u), 0x0b); // RTS/DSR and OUT2.
	}

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

	outb(serial_port, (uint8_t)ch);
}

void hal_serial_write(const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		hal_serial_write_char(data[i]);
	}
}

static bool serial_emergency_write_char(char ch) {
	for (size_t attempt = 0u; attempt < SERIAL_EMERGENCY_POLL_LIMIT; attempt++) {
		if (serial_transmit_empty()) {
			outb(serial_port, (uint8_t)ch);
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
