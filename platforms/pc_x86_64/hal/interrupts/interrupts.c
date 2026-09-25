#include <core/cpu.h>
#include <core/exception.h>
#include <core/interrupt.h>
#include <core/sched.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../cache.h"
#include "../clock.h"
#include "../paging.h"
#include "../syscall.h"
#include "../utils.h"
#include "apic.h"
#include "frame.h"
#include "pic.h"
#include "segments.h"
#include "vectors.h"

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
static bool              local_ready[64];
static uint16_t          kernel_code_selector;
static bool              global_ready;
static const struct cpu* legacy_target;

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
	return vector >= X86_IRQ_BASE && vector < X86_LAPIC_SPURIOUS_VECTOR && vector != X86_SYSCALL_VECTOR;
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
	if (vector < 48u) pic_send_eoi(vector);
	else apic_send_eoi();
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
	if (!apic_probe_isa_irqs()) return false;
	legacy_target = cpu_bsp();
	if (legacy_target == NULL) return false;
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
	if (!apic_init_local()) return false;
	x86_64_syscall_init();
	__asm__ volatile("lidt %0" : : "m"(idtr));
	if (cpu->role == CPU_ROLE_BSP) {
		legacy_target = cpu;
	}

	local_ready[cpu->index] = true;
	cpu_interrupts_set_ready(cpu, true);
	return true;
}

static bool x86_unhandled_isa_irq_valid(uint32_t id) {
	return id < X86_IRQ_COUNT && id != 0u && id != 2u;
}

static bool x86_fixed_interrupt_valid(uint32_t id) {
	return id < X86_IRQ_COUNT;
}

enum {
	X86_DELIVERY_DOMAIN_VECTOR = 0u,
	X86_MESSAGE_DOMAIN_MSI     = 0u,
};

static bool x86_event_in_range(struct hal_interrupt_event event, struct hal_interrupt_delivery_range range) {
	return event.domain == range.domain && event.id >= range.base && event.id < range.limit;
}

size_t hal_interrupt_source_domain_count(void) {
	return global_ready ? 1u : 0u;
}

bool hal_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	if (index != 0u || out_domain == NULL || !global_ready) return false;
	*out_domain =
		(struct hal_interrupt_source_domain_info){.domain = 0u, .first_source = 0u, .source_count = X86_IRQ_COUNT};
	return true;
}

bool hal_interrupt_source_resolve(uint64_t controller_register_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source) {
	if (!global_ready || out_source == NULL) return false;
	if (controller_register_address == INTERRUPT_SOURCE_CONTROLLER_PLATFORM) {
		if (!x86_fixed_interrupt_valid(local_source_id)) return false;
		*out_source = (struct hal_interrupt_source){.domain = 0u, .number = local_source_id};
		return true;
	}
	return apic_resolve_ioapic_source(controller_register_address, local_source_id, out_source);
}

bool hal_interrupt_source_claimable(const struct hal_interrupt_source* source) {
	if (!global_ready || source == NULL) return false;
	if (source->domain == 0u) return x86_unhandled_isa_irq_valid(source->number);
	return apic_ioapic_source_claimable(source);
}

bool hal_interrupt_source_configuration_supported(const struct hal_interrupt_source* source,
                                                  enum hal_interrupt_trigger         trigger,
                                                  enum hal_interrupt_polarity        polarity) {
	struct hal_interrupt_source_info info;
	if (source == NULL || trigger > HAL_INTERRUPT_TRIGGER_LEVEL || polarity > HAL_INTERRUPT_POLARITY_LOW ||
	    !hal_interrupt_source_info(source, &info))
		return false;

	/* Non-ISA I/O APIC sources have no firmware trigger/polarity metadata here. */
	if (source->domain != 0u)
		return trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE && polarity != HAL_INTERRUPT_POLARITY_FIRMWARE;

	if (apic_isa_irq_available(source->number)) return true;

	/* Legacy PIC fallback only supports edge/high semantics. */
	return trigger != HAL_INTERRUPT_TRIGGER_LEVEL && polarity != HAL_INTERRUPT_POLARITY_LOW;
}

bool hal_interrupt_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info) {
	if (source == NULL || out_info == NULL) return false;
	if (source->domain != 0u) {
		if (!apic_ioapic_source_available(source)) return false;
		*out_info = (struct hal_interrupt_source_info){
			.delivery     = {.domain = X86_DELIVERY_DOMAIN_VECTOR, .base = 48u, .limit = X86_SYSCALL_VECTOR},
			.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
			.fixed_target = NULL
        };
		return true;
	}
	if (!x86_fixed_interrupt_valid(source->number)) return false;
	bool                                routable = apic_isa_irq_available(source->number);
	struct hal_interrupt_delivery_range delivery =
		routable ? (struct hal_interrupt_delivery_range){.domain = X86_DELIVERY_DOMAIN_VECTOR,
	                                                     .base   = 48u,
	                                                     .limit  = X86_SYSCALL_VECTOR}
				 : (struct hal_interrupt_delivery_range){.domain = X86_DELIVERY_DOMAIN_VECTOR,
	                                                     .base   = X86_IRQ_BASE + source->number,
	                                                     .limit  = X86_IRQ_BASE + source->number + 1u};
	const struct cpu* fixed_target = routable ? NULL : legacy_target;
	if (!routable && fixed_target == NULL) return false;
	*out_info = (struct hal_interrupt_source_info){.delivery     = delivery,
	                                               .target_kind  = routable ? HAL_INTERRUPT_TARGET_ROUTABLE
	                                                                        : HAL_INTERRUPT_TARGET_FIXED,
	                                               .fixed_target = fixed_target};
	return true;
}

bool hal_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	struct hal_interrupt_source_info info;
	if (target == NULL || target->index >= 64u || !local_ready[target->index] ||
	    !hal_interrupt_source_info(source, &info))
		return false;
	if (info.target_kind == HAL_INTERRUPT_TARGET_FIXED) return target == info.fixed_target;
	return target->arch_id <= 255u;
}

bool hal_interrupt_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	struct hal_interrupt_source_info info;
	uint32_t                         route;
	uintptr_t                        registers;
	if (state == NULL || state->initialized || delivery == NULL || !hal_interrupt_source_info(source, &info) ||
	    !x86_event_in_range(delivery->event, info.delivery) ||
	    !hal_interrupt_source_target_supported(source, delivery->target) ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity > HAL_INTERRUPT_POLARITY_LOW)
		return false;
	bool routed = source->domain == 0u ? apic_route_isa_irq(source->number,
	                                                        delivery->event.id,
	                                                        (uint32_t)delivery->target->arch_id,
	                                                        delivery->trigger,
	                                                        delivery->polarity,
	                                                        &route,
	                                                        &registers)
	                                   : apic_route_ioapic_source(source,
	                                                              delivery->event.id,
	                                                              (uint32_t)delivery->target->arch_id,
	                                                              delivery->trigger,
	                                                              delivery->polarity,
	                                                              &route,
	                                                              &registers);
	if (info.target_kind == HAL_INTERRUPT_TARGET_ROUTABLE && routed) {
		*state = (struct hal_interrupt_source_state){.source           = *source,
		                                             .ioapic_route     = route,
		                                             .ioapic_registers = registers,
		                                             .initialized      = true,
		                                             .masked           = true};
		return true;
	}
	if (info.target_kind != HAL_INTERRUPT_TARGET_FIXED || delivery->target->role != CPU_ROLE_BSP ||
	    delivery->trigger == HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity == HAL_INTERRUPT_POLARITY_LOW)
		return false;
	pic_mask_irq(source->number);
	*state =
		(struct hal_interrupt_source_state){.source = *source, .uses_pic = true, .initialized = true, .masked = true};
	return true;
}

bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized) return false;
	if (state->uses_pic) pic_mask_irq(state->source.number);
	else if (!apic_set_isa_irq_mask(state->ioapic_registers, state->ioapic_route, true)) return false;
	state->masked = true;
	return true;
}

bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized) return false;
	if (state->uses_pic) pic_unmask_irq(state->source.number);
	else if (!apic_set_isa_irq_mask(state->ioapic_registers, state->ioapic_route, false)) return false;
	state->masked = false;
	return true;
}

bool hal_interrupt_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!hal_interrupt_source_mask(state)) return false;
	state->initialized = false;
	return true;
}

size_t hal_interrupt_message_range_count(void) {
	return global_ready ? 2u : 0u;
}

bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (index >= 2u || out_range == NULL || !global_ready) return false;
	*out_range = index == 0u
	                    ? (struct hal_interrupt_message_range){
		                      .domain   = X86_MESSAGE_DOMAIN_MSI,
		                      .delivery = {.domain = X86_DELIVERY_DOMAIN_VECTOR,
		                                   .base   = 48u,
		                                   .limit  = X86_SYSCALL_VECTOR}}
	                    : (struct hal_interrupt_message_range){
		                      .domain   = X86_MESSAGE_DOMAIN_MSI,
		                      .delivery = {.domain = X86_DELIVERY_DOMAIN_VECTOR,
		                                   .base   = X86_SYSCALL_VECTOR + 1u,
		                                   .limit  = X86_LAPIC_WAKE_VECTOR}};
	return true;
}

bool hal_interrupt_message_resolve(uint64_t controller_register_address, uint32_t producer_id,
                                   struct hal_interrupt_message_context* out_context) {
	if (!global_ready || controller_register_address != UINT64_MAX || producer_id != UINT32_MAX ||
	    out_context == NULL || hal_interrupt_message_range_count() == 0u)
		return false;
	*out_context = (struct hal_interrupt_message_context){.domain = X86_MESSAGE_DOMAIN_MSI};
	return true;
}

bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	return global_ready && domain == X86_MESSAGE_DOMAIN_MSI && source == NULL && target != NULL &&
	       target->index < 64u && target->arch_id <= 255u && local_ready[target->index];
}

static bool x86_message_request_valid(const struct hal_interrupt_message_request* request) {
	if (request == NULL || !hal_interrupt_message_target_supported(request->domain, request->source, request->target))
		return false;
	for (size_t index = 0u; index < hal_interrupt_message_range_count(); ++index) {
		struct hal_interrupt_message_range range;
		if (hal_interrupt_message_range_at(index, &range) && range.domain == request->domain &&
		    x86_event_in_range(request->event, range.delivery))
			return true;
	}
	return false;
}

bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	if (state == NULL || state->initialized || out_message == NULL || !x86_message_request_valid(request)) return false;
	*out_message       = (struct hal_interrupt_message){.address = 0xfee00000ull | (request->target->arch_id << 12),
	                                                    .data    = request->event.id};
	state->initialized = true;
	return true;
}

bool hal_interrupt_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	state->initialized = false;
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
	if (vector == 2u) {
		bool handled = x86_64_cache_handle_sync_nmi();
		handled |= x86_64_paging_handle_tlb_nmi();
		if (handled) {
			cpu_leave_exception();
			return;
		}
	}
	if (vector == X86_LAPIC_WAKE_VECTOR) {
		apic_send_eoi();
		return;
	}
	if (is_external_irq(vector)) {
		bool pic_spurious =
			vector >= X86_IRQ_BASE && vector < X86_IRQ_BASE + X86_IRQ_COUNT && pic_is_spurious_irq((unsigned)vector);
		bool handled = false;
		if (!pic_spurious) {
			handled = clock_handle_irq((unsigned)vector);
			handled |= interrupt_handle_event(
				(struct hal_interrupt_event){.domain = X86_DELIVERY_DOMAIN_VECTOR, .id = (uint32_t)vector});
		}
		if (!handled && !pic_spurious && vector < X86_IRQ_BASE + X86_IRQ_COUNT) {
			uint32_t id = (uint32_t)(vector - X86_IRQ_BASE);
			if (x86_unhandled_isa_irq_valid(id)) pic_mask_irq(id);
		}
		interrupt_send_eoi((unsigned)vector);
		return;
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
