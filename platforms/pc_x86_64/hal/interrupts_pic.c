#include <stddef.h>

#include "interrupts_private.h"

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

void pic_send_eoi(unsigned vector) {
	if (vector < X86_IRQ_BASE || vector >= X86_IRQ_BASE + X86_IRQ_COUNT) return;
	if (vector >= X86_IRQ_BASE + 8u) outb(X86_PIC2_CMD, X86_PIC_EOI);
	outb(X86_PIC1_CMD, X86_PIC_EOI);
}

bool pit_init(uint32_t frequency_hz, uint32_t* actual_frequency_hz) {
	uint32_t divisor;

	if (frequency_hz == 0u) return false;

	divisor = X86_PIT_INPUT_HZ / frequency_hz;
	if (divisor == 0u || divisor > 0xffffu) return false;

	outb(X86_PIT_COMMAND, 0x36u);
	outb(X86_PIT_CHANNEL0, (uint8_t)(divisor & 0xffu));
	outb(X86_PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xffu));
	if (actual_frequency_hz != NULL) {
		*actual_frequency_hz = X86_PIT_INPUT_HZ / divisor;
	}
	return true;
}
