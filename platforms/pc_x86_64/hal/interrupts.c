#include <core/cpu.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "interrupts_private.h"

#define X86_GDT_KERNEL_CODE_SELECTOR 0x08u
#define X86_GDT_KERNEL_DATA_SELECTOR 0x10u
#define X86_GDT_TSS_SELECTOR 0x18u
#define X86_EXCEPTION_STACK_SIZE 0x4000u
#define X86_EXCEPTION_IST_INDEX 1u

static bool x86_page_fault_is_not_present(uint64_t error_code) {
	return (error_code & 0x1u) == 0;
}

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

struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

struct tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist1;
	uint64_t ist2;
	uint64_t ist3;
	uint64_t ist4;
	uint64_t ist5;
	uint64_t ist6;
	uint64_t ist7;
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

extern void (*x86_64_interrupt_stub_table[])(void);

static struct idt_entry idt[256];
static uint64_t         gdt[64][5];
static struct tss64     x86_tss[64];
static _Alignas(16) uint8_t x86_exception_stack[64][X86_EXCEPTION_STACK_SIZE];
static bool     local_ready[64];
static uint16_t kernel_code_selector;
static bool     global_ready;

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

bool irq_enabled(void) {
	uint64_t flags;

	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
	return (flags & (1ull << 9)) != 0;
}

void irq_enable_local(void) {
	__asm__ volatile("sti" : : : "memory");
}

void irq_disable_local(void) {
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

static void x86_load_segments_and_tss(size_t cpu_index) {
	struct gdtr gdtr = {
		.limit = (uint16_t)(sizeof(gdt[cpu_index]) - 1u),
		.base  = (uint64_t)(uintptr_t)gdt[cpu_index],
	};
	uint16_t tss_selector = X86_GDT_TSS_SELECTOR;

	__asm__ volatile(
		"lgdt %0\n\t"
		"movw %1, %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		"pushq %2\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		"ltr %3"
		:
		: "m"(gdtr), "i"(X86_GDT_KERNEL_DATA_SELECTOR), "i"((uint64_t)X86_GDT_KERNEL_CODE_SELECTOR), "r"(tss_selector)
		: "rax", "memory");
}

static void x86_setup_exception_stack(size_t cpu_index) {
	uint64_t base;
	uint64_t limit;

	gdt[cpu_index][0] = 0u;
	gdt[cpu_index][1] = 0x00af9a000000ffffull;
	gdt[cpu_index][2] = 0x00af92000000ffffull;

	memset(&x86_tss[cpu_index], 0, sizeof(x86_tss[cpu_index]));
	x86_tss[cpu_index].ist1       = (uint64_t)(uintptr_t)(x86_exception_stack[cpu_index] + X86_EXCEPTION_STACK_SIZE);
	x86_tss[cpu_index].iomap_base = (uint16_t)sizeof(x86_tss[cpu_index]);

	base              = (uint64_t)(uintptr_t)&x86_tss[cpu_index];
	limit             = (uint64_t)(sizeof(x86_tss[cpu_index]) - 1u);
	gdt[cpu_index][3] = (limit & 0xffffu) | ((base & 0xffffull) << 16) | (((base >> 16) & 0xffull) << 32) |
	                    ((uint64_t)0x89u << 40) | (((limit >> 16) & 0x0full) << 48) | (((base >> 24) & 0xffull) << 56);
	gdt[cpu_index][4] = base >> 32;

	x86_load_segments_and_tss(cpu_index);
}

static void idt_set_entry(unsigned vector, void (*handler)(void)) {
	uint64_t address = (uint64_t)(uintptr_t)handler;

	idt[vector] = (struct idt_entry){
		.offset_low      = (uint16_t)(address & 0xffffu),
		.selector        = kernel_code_selector,
		.ist             = X86_EXCEPTION_IST_INDEX,
		.type_attributes = 0x8eu,
		.offset_mid      = (uint16_t)((address >> 16) & 0xffffu),
		.offset_high     = (uint32_t)(address >> 32),
		.reserved        = 0u,
	};
}

bool hal_interrupts_init_global(void) {
	if (global_ready) return true;
	irq_disable_local();
	kernel_code_selector = X86_GDT_KERNEL_CODE_SELECTOR;

	for (unsigned vector = 0; vector < 256; vector++) {
		idt_set_entry(vector, x86_64_interrupt_stub_table[vector]);
	}

	struct idtr idtr = {
		.limit = (uint16_t)(sizeof(idt) - 1u),
		.base  = (uint64_t)(uintptr_t)idt,
	};

	__asm__ volatile("lidt %0" : : "m"(idtr));
	pic_init();
	global_ready = true;
	printf("kernel: x86_64 interrupt globals installed (cs=0x%04x)\n", kernel_code_selector);
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	struct idtr idtr = {
		.limit = (uint16_t)(sizeof(idt) - 1u),
		.base  = (uint64_t)(uintptr_t)idt,
	};

	if (!global_ready || cpu == NULL || cpu->index >= 64u) return false;
	if (local_ready[cpu->index]) return true;

	irq_disable_local();
	x86_setup_exception_stack(cpu->index);
	__asm__ volatile("lidt %0" : : "m"(idtr));
	if (cpu->role == CPU_ROLE_BSP) {
		irq_enable_local();
	}

	local_ready[cpu->index] = true;
	cpu_interrupts_set_ready(cpu, true);
	if (cpu->role == CPU_ROLE_BSP) {
		printf("kernel: x86_64 local interrupts ready on cpu%zu (ist=0x%016llx)\n",
		       cpu->index,
		       (unsigned long long)x86_tss[cpu->index].ist1);
	}
	return true;
}

void x86_64_maybe_preempt_on_interrupt_exit(void) {
	(void)sched_handle_interrupt_exit();
}

void x86_64_handle_interrupt(const struct interrupt_frame* frame) {
	unsigned long long vector = frame->vector;
	uint64_t           fault_addr;
	bool               trap_context = !is_external_irq(vector);

	if (trap_context) cpu_enter_exception();
	if (is_external_irq(vector)) {
		bool handled = clock_handle_irq((unsigned)vector);
		interrupt_send_eoi((unsigned)vector);
		if (handled) {
			return;
		}
	}

	fault_addr = vector == 14u ? read_cr2() : 0;
	if (vector == 14u && x86_page_fault_is_not_present(frame->error_code) &&
	    vmm_resolve_page_fault((uintptr_t)fault_addr)) {
		if (trap_context) cpu_leave_exception();
		return;
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
		       fault_addr,
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
