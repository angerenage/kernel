#include <core/cpu.h>
#include <core/exception.h>
#include <core/sched.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "interrupts_private.h"

#define X86_EXCEPTION_STACK_SIZE 0x4000u
#define X86_EXCEPTION_IST_INDEX 1u
#define X86_IDT_NO_IST 0u
#define X86_IDT_INTERRUPT_GATE 0x8eu
#define X86_IDT_USER_INTERRUPT_GATE 0xeeu

static bool x86_page_fault_is_not_present(uint64_t error_code) {
	return (error_code & 0x1u) == 0;
}

static bool x86_page_fault_from_user(uint64_t error_code) {
	return (error_code & (1u << 2)) != 0;
}

static bool x86_page_fault_is_write(uint64_t error_code) {
	return (error_code & (1u << 1)) != 0;
}

static bool x86_page_fault_is_instruction(uint64_t error_code) {
	return (error_code & (1u << 4)) != 0;
}

static enum core_exception_kind x86_page_fault_kind(uint64_t error_code) {
	return x86_page_fault_is_not_present(error_code) ? CORE_EXCEPTION_PAGE_FAULT_NOT_PRESENT
	                                                 : CORE_EXCEPTION_PAGE_FAULT_PROTECTION;
}

static enum core_exception_access x86_page_fault_access(uint64_t error_code) {
	if (x86_page_fault_is_instruction(error_code)) return CORE_EXCEPTION_ACCESS_EXEC;
	return x86_page_fault_is_write(error_code) ? CORE_EXCEPTION_ACCESS_WRITE : CORE_EXCEPTION_ACCESS_READ;
}

static bool x86_exception_from_user(const struct interrupt_frame* frame) {
	return (frame->cs & 0x3u) == 3u;
}

static bool x86_exception_kind(unsigned long long vector, enum core_exception_kind* out_kind) {
	if (!out_kind) return false;
	switch (vector) {
	case 0:
		*out_kind = CORE_EXCEPTION_ARITHMETIC_DIVIDE_BY_ZERO;
		return true;
	case 1:
		*out_kind = CORE_EXCEPTION_DEBUG;
		return true;
	case 3:
		*out_kind = CORE_EXCEPTION_BREAKPOINT;
		return true;
	case 4:
		*out_kind = CORE_EXCEPTION_ARITHMETIC_OVERFLOW;
		return true;
	case 5:
		*out_kind = CORE_EXCEPTION_ARITHMETIC_BOUND_RANGE;
		return true;
	case 6:
		*out_kind = CORE_EXCEPTION_INSTRUCTION_ILLEGAL;
		return true;
	case 13:
		*out_kind = CORE_EXCEPTION_PRIVILEGE_GENERAL_PROTECTION;
		return true;
	case 16:
		*out_kind = CORE_EXCEPTION_FLOATING_POINT;
		return true;
	case 19:
		*out_kind = CORE_EXCEPTION_FLOATING_POINT_SIMD;
		return true;
	case 17:
		*out_kind = CORE_EXCEPTION_ALIGNMENT;
		return true;
	default:
		return false;
	}
}

static bool x86_handle_user_exception(unsigned long long vector, const struct interrupt_frame* frame) {
	enum core_exception_kind kind;

	if (vector >= 32u) return false;
	if (vector == 14u) return false;
	if (!x86_exception_from_user(frame)) return false;
	if (!x86_exception_kind(vector, &kind)) return false;
	return core_handle_exception(kind, CORE_EXCEPTION_ACCESS_UNKNOWN, 0u, true);
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
static uint64_t         gdt[64][8];
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

static uint8_t x86_ist_for_vector(unsigned vector) {
	switch (vector) {
	case 2u:  /* NMI */
	case 8u:  /* Double fault */
	case 18u: /* Machine check */
		return X86_EXCEPTION_IST_INDEX;
	default:
		return X86_IDT_NO_IST;
	}
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

static bool x86_setup_exception_stack(struct cpu* cpu) {
	size_t   cpu_index;
	uint64_t base;
	uint64_t limit;

	if (cpu == NULL || cpu->index >= 64u || cpu->kernel_entry_stack_top == 0u) return false;
	cpu_index = cpu->index;

	gdt[cpu_index][0] = 0u;
	gdt[cpu_index][1] = 0x00af9a000000ffffull;
	gdt[cpu_index][2] = 0x00af92000000ffffull;
	gdt[cpu_index][3] = 0x00cffb000000ffffull;
	gdt[cpu_index][4] = 0x00cff3000000ffffull;
	gdt[cpu_index][5] = 0x00affa000000ffffull;

	memset(&x86_tss[cpu_index], 0, sizeof(x86_tss[cpu_index]));
	x86_tss[cpu_index].rsp0       = (uint64_t)cpu->kernel_entry_stack_top;
	x86_tss[cpu_index].ist1       = (uint64_t)(uintptr_t)(x86_exception_stack[cpu_index] + X86_EXCEPTION_STACK_SIZE);
	x86_tss[cpu_index].iomap_base = (uint16_t)sizeof(x86_tss[cpu_index]);

	base              = (uint64_t)(uintptr_t)&x86_tss[cpu_index];
	limit             = (uint64_t)(sizeof(x86_tss[cpu_index]) - 1u);
	gdt[cpu_index][6] = (limit & 0xffffu) | ((base & 0xffffull) << 16) | (((base >> 16) & 0xffull) << 32) |
	                    ((uint64_t)0x89u << 40) | (((limit >> 16) & 0x0full) << 48) | (((base >> 24) & 0xffull) << 56);
	gdt[cpu_index][7] = base >> 32;

	x86_load_segments_and_tss(cpu_index);
	return true;
}

static void idt_set_entry(unsigned vector, void (*handler)(void), uint8_t ist, uint8_t type_attributes) {
	uint64_t address = (uint64_t)(uintptr_t)handler;

	idt[vector] = (struct idt_entry){
		.offset_low      = (uint16_t)(address & 0xffffu),
		.selector        = kernel_code_selector,
		.ist             = ist,
		.type_attributes = type_attributes,
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
		idt_set_entry(vector, x86_64_interrupt_stub_table[vector], x86_ist_for_vector(vector), X86_IDT_INTERRUPT_GATE);
	}
	idt_set_entry(X86_SYSCALL_VECTOR,
	              x86_64_interrupt_stub_table[X86_SYSCALL_VECTOR],
	              X86_IDT_NO_IST,
	              X86_IDT_USER_INTERRUPT_GATE);

	struct idtr idtr = {
		.limit = (uint16_t)(sizeof(idt) - 1u),
		.base  = (uint64_t)(uintptr_t)idt,
	};

	__asm__ volatile("lidt %0" : : "m"(idtr));
	pic_init();
	global_ready = true;
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
	if (!x86_setup_exception_stack(cpu)) return false;
	if (apic_ipi_ready() && !apic_init_local()) return false;
	x86_64_syscall_init();
	__asm__ volatile("lidt %0" : : "m"(idtr));
	if (cpu->role == CPU_ROLE_BSP) {
		irq_enable_local();
	}

	local_ready[cpu->index] = true;
	cpu_interrupts_set_ready(cpu, true);
	return true;
}

void x86_64_prepare_user_return(void) {
	struct cpu* cpu = cpu_current();

	sched_finish_context_switch();
	if (cpu == NULL || cpu->index >= 64u || cpu->kernel_entry_stack_top == 0u) {
		hcf();
	}
	x86_tss[cpu->index].rsp0 = (uint64_t)cpu->kernel_entry_stack_top;
	__asm__ volatile("" : : : "memory");
}

void x86_64_maybe_preempt_on_interrupt_exit(void) {
	(void)sched_handle_interrupt_exit();
}

void x86_64_handle_interrupt(struct interrupt_frame* frame) {
	unsigned long long vector = frame->vector;
	uint64_t           fault_addr;
	bool               trap_context = !is_external_irq(vector);

	if (x86_64_handle_syscall(frame)) return;
	/* LAPIC spurious interrupts require neither an EOI nor fatal exception handling. */
	if (vector == X86_LAPIC_SPURIOUS_VECTOR) return;
	if (trap_context) cpu_enter_exception();
	if (vector == 2u && x86_64_paging_handle_tlb_nmi()) {
		cpu_leave_exception();
		return;
	}
	if (vector == X86_LAPIC_WAKE_VECTOR) {
		apic_send_eoi();
		cpu_leave_exception();
		return;
	}
	if (is_external_irq(vector)) {
		bool handled = clock_handle_irq((unsigned)vector);
		interrupt_send_eoi((unsigned)vector);
		if (handled) {
			return;
		}
	}

	fault_addr = vector == 14u ? read_cr2() : 0;
	if (vector == 14u) {
		if (trap_context) cpu_leave_exception();
		if (core_handle_exception(x86_page_fault_kind(frame->error_code),
		                          x86_page_fault_access(frame->error_code),
		                          (uintptr_t)fault_addr,
		                          x86_page_fault_from_user(frame->error_code))) {
			return;
		}
		if (trap_context) cpu_enter_exception();
	}

	if (vector < 32u) {
		if (trap_context) cpu_leave_exception();
		if (x86_handle_user_exception(vector, frame)) return;
		if (trap_context) cpu_enter_exception();
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
