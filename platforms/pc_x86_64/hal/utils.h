#include <stdint.h>

static inline uint16_t current_code_selector(void) {
	uint16_t selector;

	__asm__ volatile("mov %%cs, %0" : "=r"(selector));
	return selector;
}

static inline void io_wait(void) {
	__asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t value;

	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint64_t read_cr2(void) {
	uint64_t value;

	__asm__ volatile("mov %%cr2, %0" : "=r"(value));
	return value;
}

static inline uint64_t read_msr(uint32_t msr) {
	uint32_t lo;
	uint32_t hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
	uint32_t lo = (uint32_t)(value & 0xffffffffu);
	uint32_t hi = (uint32_t)(value >> 32);

	__asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
