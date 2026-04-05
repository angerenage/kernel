#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

struct idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t  ist;
	uint8_t  type_attributes;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

extern void (*x86_64_interrupt_stub_table[])(void);

static struct idt_entry idt[256];
static uint16_t         kernel_code_selector;
static bool             traps_ready;

static const char* const exception_names[32] = {
	"Divide Error",
	"Debug",
	"Non-Maskable Interrupt",
	"Breakpoint",
	"Overflow",
	"Bound Range Exceeded",
	"Invalid Opcode",
	"Device Not Available",
	"Double Fault",
	"Coprocessor Segment Overrun",
	"Invalid TSS",
	"Segment Not Present",
	"Stack-Segment Fault",
	"General Protection Fault",
	"Page Fault",
	"Reserved",
	"x87 Floating-Point Exception",
	"Alignment Check",
	"Machine Check",
	"SIMD Floating-Point Exception",
	"Virtualization Exception",
	"Control Protection Exception",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Hypervisor Injection Exception",
	"VMM Communication Exception",
	"Security Exception",
	"Reserved",
};

static bool is_external_irq(unsigned long long vector) {
	return vector >= X86_IRQ_BASE && vector < X86_IRQ_BASE + X86_IRQ_COUNT;
}

static void interrupts_enable(void) {
	__asm__ volatile("sti" : : : "memory");
}

static void interrupts_disable(void) {
	__asm__ volatile("cli" : : : "memory");
}

static void interrupt_send_eoi(unsigned vector) {
	if (!is_external_irq(vector)) return;

	if (apic_is_active()) {
		apic_send_eoi();
		return;
	}

	pic_send_eoi(vector);
}

static void idt_set_entry(unsigned vector, void (*handler)(void)) {
	uint64_t address = (uint64_t)(uintptr_t)handler;

	idt[vector] = (struct idt_entry){
		.offset_low      = (uint16_t)(address & 0xffffu),
		.selector        = kernel_code_selector,
		.ist             = 0u,
		.type_attributes = 0x8eu,
		.offset_mid      = (uint16_t)((address >> 16) & 0xffffu),
		.offset_high     = (uint32_t)(address >> 32),
		.reserved        = 0u,
	};
}

void hal_interrupts_init(void) {
	if (traps_ready) return;

	interrupts_disable();
	kernel_code_selector = current_code_selector();

	for (unsigned vector = 0; vector < 256; vector++) {
		idt_set_entry(vector, x86_64_interrupt_stub_table[vector]);
	}

	struct idtr idtr = {
		.limit = (uint16_t)(sizeof(idt) - 1u),
		.base  = (uint64_t)(uintptr_t)idt,
	};

	__asm__ volatile("lidt %0" : : "m"(idtr));
	pic_init();
	interrupts_enable();

	traps_ready = true;
	printf("kernel: x86_64 idt installed (cs=0x%04x)\n", kernel_code_selector);
}

void x86_64_handle_interrupt(const struct interrupt_frame* frame) {
	unsigned long long vector = frame->vector;

	if (is_external_irq(vector)) {
		bool handled = clock_handle_irq((unsigned)vector);
		interrupt_send_eoi((unsigned)vector);
		if (handled) return;
	}

	if (vector < 32u) {
		printf("kernel: exception %llu (%s)\n", vector, exception_names[vector]);
	}
	else {
		printf("kernel: unexpected interrupt %llu\n", vector);
	}

	if (vector == 14u) {
		printf("  error=0x%016llx cr2=0x%016llx rip=0x%016llx cs=0x%016llx rflags=0x%016llx\n",
		       frame->error_code,
		       read_cr2(),
		       frame->rip,
		       frame->cs,
		       frame->rflags);
	}
	else {
		printf("  error=0x%016llx rip=0x%016llx cs=0x%016llx rflags=0x%016llx\n",
		       frame->error_code,
		       frame->rip,
		       frame->cs,
		       frame->rflags);
	}
	printf(
		"  rax=0x%016llx rbx=0x%016llx rcx=0x%016llx rdx=0x%016llx\n", frame->rax, frame->rbx, frame->rcx, frame->rdx);
	printf("  rbp=0x%016llx rdi=0x%016llx rsi=0x%016llx\n", frame->rbp, frame->rdi, frame->rsi);
	printf("  r8 =0x%016llx r9 =0x%016llx r10=0x%016llx r11=0x%016llx\n", frame->r8, frame->r9, frame->r10, frame->r11);
	printf(
		"  r12=0x%016llx r13=0x%016llx r14=0x%016llx r15=0x%016llx\n", frame->r12, frame->r13, frame->r14, frame->r15);

	hcf();
}
