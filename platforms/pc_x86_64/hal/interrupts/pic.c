#include "pic.h"

#include <stddef.h>

#include "../utils.h"
#include "vectors.h"

#define X86_PIC1_CMD 0x20u
#define X86_PIC1_DATA 0x21u
#define X86_PIC2_CMD 0xa0u
#define X86_PIC2_DATA 0xa1u
#define X86_PIC_EOI 0x20u
#define X86_PIC_OCW3_READ_ISR 0x0bu
#define X86_ICW1_INIT 0x10u
#define X86_ICW1_ICW4 0x01u
#define X86_ICW4_8086 0x01u
#define X86_PIT_CHANNEL0 0x40u
#define X86_PIT_COMMAND 0x43u
#define X86_PIT_INPUT_HZ 1193182u

static void pic_remap(void) {
	uint8_t pic1_mask = inb(X86_PIC1_DATA);
	uint8_t pic2_mask = inb(X86_PIC2_DATA);

	outb(X86_PIC1_CMD, X86_ICW1_INIT | X86_ICW1_ICW4);
	io_wait();
	outb(X86_PIC2_CMD, X86_ICW1_INIT | X86_ICW1_ICW4);
	io_wait();

	outb(X86_PIC1_DATA, (uint8_t)X86_IRQ_BASE);
	io_wait();
	outb(X86_PIC2_DATA, (uint8_t)(X86_IRQ_BASE + 8u));
	io_wait();

	outb(X86_PIC1_DATA, 4u);
	io_wait();
	outb(X86_PIC2_DATA, 2u);
	io_wait();

	outb(X86_PIC1_DATA, X86_ICW4_8086);
	io_wait();
	outb(X86_PIC2_DATA, X86_ICW4_8086);
	io_wait();

	outb(X86_PIC1_DATA, pic1_mask);
	outb(X86_PIC2_DATA, pic2_mask);
}

static void pic_mask_all(void) {
	outb(X86_PIC1_DATA, 0xffu);
	outb(X86_PIC2_DATA, 0xffu);
}

void pic_init(void) {
	pic_remap();
	pic_mask_all();
}

void pic_mask_irq(unsigned irq) {
	uint16_t port;
	uint8_t  mask;
	unsigned line;

	if (irq >= X86_IRQ_COUNT) return;

	if (irq < 8u) {
		port = X86_PIC1_DATA;
		line = irq;
	}
	else {
		port = X86_PIC2_DATA;
		line = irq - 8u;
	}

	mask = (uint8_t)(inb(port) | (1u << line));
	outb(port, mask);
}

void pic_unmask_irq(unsigned irq) {
	uint16_t port;
	uint8_t  mask;
	unsigned line;

	if (irq >= X86_IRQ_COUNT) return;

	if (irq < 8u) {
		port = X86_PIC1_DATA;
		line = irq;
	}
	else {
		port = X86_PIC2_DATA;
		line = irq - 8u;
	}

	mask = (uint8_t)(inb(port) & ~(1u << line));
	outb(port, mask);

	if (irq >= 8u) {
		mask = (uint8_t)(inb(X86_PIC1_DATA) & ~(1u << 2));
		outb(X86_PIC1_DATA, mask);
	}
}

static uint8_t pic_read_isr(uint16_t command_port) {
	outb(command_port, X86_PIC_OCW3_READ_ISR);
	return inb(command_port);
}

bool pic_is_spurious_irq(unsigned vector) {
	if (vector == X86_IRQ_BASE + 7u) return (pic_read_isr(X86_PIC1_CMD) & (1u << 7u)) == 0u;
	if (vector == X86_IRQ_BASE + 15u) return (pic_read_isr(X86_PIC2_CMD) & (1u << 7u)) == 0u;
	return false;
}

void pic_send_eoi(unsigned vector) {
	if (vector < X86_IRQ_BASE || vector >= X86_IRQ_BASE + X86_IRQ_COUNT) return;
	if (vector == X86_IRQ_BASE + 7u && pic_is_spurious_irq(vector)) return;
	if (vector == X86_IRQ_BASE + 15u && pic_is_spurious_irq(vector)) {
		outb(X86_PIC1_CMD, X86_PIC_EOI);
		return;
	}
	if (vector >= X86_IRQ_BASE + 8u) outb(X86_PIC2_CMD, X86_PIC_EOI);
	outb(X86_PIC1_CMD, X86_PIC_EOI);
}

bool pit_init(uint32_t frequency_hz, uint32_t* actual_frequency_hz) {
	uint32_t divisor;

	if (frequency_hz == 0u) return false;

	divisor = X86_PIT_INPUT_HZ / frequency_hz;
	if (divisor == 0u || divisor > 0xffffu) return false;

	outb(X86_PIT_COMMAND, 0x34u);
	outb(X86_PIT_CHANNEL0, (uint8_t)(divisor & 0xffu));
	outb(X86_PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xffu));
	if (actual_frequency_hz != NULL) {
		*actual_frequency_hz = X86_PIT_INPUT_HZ / divisor;
	}
	return true;
}
