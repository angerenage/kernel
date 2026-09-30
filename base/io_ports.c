#include <base/io_port.h>

#if !defined(IO_PORT_TEST)

uint8_t io_read8(uint16_t port) {
	uint8_t value;
	__asm__ volatile("inb %w1, %b0" : "=a"(value) : "Nd"(port));
	return value;
}

uint16_t io_read16(uint16_t port) {
	uint16_t value;
	__asm__ volatile("inw %w1, %w0" : "=a"(value) : "Nd"(port));
	return value;
}

uint32_t io_read32(uint16_t port) {
	uint32_t value;
	__asm__ volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

void io_write8(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %b0, %w1" : : "a"(value), "Nd"(port));
}

void io_write16(uint16_t port, uint16_t value) {
	__asm__ volatile("outw %w0, %w1" : : "a"(value), "Nd"(port));
}

void io_write32(uint16_t port, uint32_t value) {
	__asm__ volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
}

#endif
