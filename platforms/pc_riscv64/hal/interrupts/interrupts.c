#include "interrupts.h"

#include <base/process.h>
#include <core/address_space.h>
#include <core/cpu.h>
#include <core/exception.h>
#include <core/sched.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../clock.h"
#include "../syscall.h"
#include "aplic.h"
#include "frame.h"
#include "imsic.h"
#include "plic.h"

#define RISCV64_SIE_SSIE (1ull << 1)
#define RISCV64_SIE_STIE (1ull << 5)
#define RISCV64_SIE_SEIE (1ull << 9)
#define RISCV64_SIP_SSIP (1ull << 1)
#define RISCV64_SCAUSE_SUPERVISOR_SOFTWARE 1u
#define RISCV64_SCAUSE_SUPERVISOR_EXTERNAL 9u

extern void exception_entry(void);

static bool global_ready;
static bool local_ready[64];
struct riscv64_exception_entry_state {
	uintptr_t old_sp;
	uintptr_t saved_t1;
	uintptr_t saved_t2;
	uintptr_t scratch;
	uintptr_t kernel_stack_top;
} riscv64_exception_entry_state[64];

_Static_assert(offsetof(struct riscv64_exception_entry_state, kernel_stack_top) == 32u,
               "riscv64 exception entry assembly kernel_stack_top offset mismatch");

static inline uint64_t read_sie(void) {
	uint64_t value;
	__asm__ volatile("csrr %0, sie" : "=r"(value));
	return value;
}

static inline void write_sscratch(uint64_t value) {
	__asm__ volatile("csrw sscratch, %0" : : "r"(value) : "memory");
}

static inline void write_sie(uint64_t value) {
	__asm__ volatile("csrw sie, %0" : : "r"(value) : "memory");
}

static inline void clear_software_interrupt(void) {
	__asm__ volatile("csrc sip, %0" : : "r"(RISCV64_SIP_SSIP) : "memory");
}

bool irq_enabled(void) {
	uint64_t sstatus;

	__asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
	return (sstatus & (1ull << 1)) != 0;
}

void irq_disable_local(void) {
	__asm__ volatile("csrc sstatus, %0" : : "r"(1ull << 1) : "memory");
}

void irq_enable_local(void) {
	__asm__ volatile("csrs sstatus, %0" : : "r"(1ull << 1) : "memory");
}

void riscv64_sync_external_interrupt_local(void) {
	uint64_t sie;

	sie = read_sie();
	if (riscv64_plic_cpu_has_enabled_sources(cpu_current()) || riscv64_aplic_cpu_has_interface(cpu_current()) ||
	    riscv64_imsic_cpu_has_interface(cpu_current()))
		sie |= RISCV64_SIE_SEIE;
	else sie &= ~RISCV64_SIE_SEIE;
	write_sie(sie);
}

size_t hal_interrupt_source_domain_count(void) {
	if (!global_ready) return 0u;
	struct hal_interrupt_source_domain_info domain;
	return (riscv64_plic_source_domain_at(&domain) ? 1u : 0u) + (riscv64_aplic_source_domain_at(&domain) ? 1u : 0u);
}

bool hal_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	if (out_domain == NULL || !global_ready) return false;
	struct hal_interrupt_source_domain_info domain;
	if (riscv64_plic_source_domain_at(&domain)) {
		if (index == 0u) {
			*out_domain = domain;
			return true;
		}
		index--;
	}
	return index == 0u && riscv64_aplic_source_domain_at(out_domain);
}

bool hal_interrupt_source_resolve(uint64_t controller_register_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source) {
	return global_ready && (riscv64_plic_source_resolve(controller_register_address, local_source_id, out_source) ||
	                        riscv64_aplic_source_resolve(controller_register_address, local_source_id, out_source));
}

bool hal_interrupt_source_configuration_supported(const struct hal_interrupt_source* source,
                                                  enum hal_interrupt_trigger         trigger,
                                                  enum hal_interrupt_polarity        polarity) {
	struct hal_interrupt_source_info info;
	if (source == NULL || trigger > HAL_INTERRUPT_TRIGGER_LEVEL || polarity > HAL_INTERRUPT_POLARITY_LOW) return false;

	if (source->domain == 1u) {
		if (!riscv64_plic_source_info(source, &info)) return false;
		return trigger == HAL_INTERRUPT_TRIGGER_FIRMWARE && polarity == HAL_INTERRUPT_POLARITY_FIRMWARE;
	}

	if (!riscv64_aplic_source_info(source, &info)) return false;
	return trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE && polarity != HAL_INTERRUPT_POLARITY_FIRMWARE;
}

bool hal_interrupt_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info) {
	if (source == NULL) return false;
	if (source->domain == 1u) return riscv64_plic_source_info(source, out_info);
	return riscv64_aplic_source_info(source, out_info);
}

bool hal_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	if (source == NULL) return false;
	if (source->domain == 1u) return riscv64_plic_source_target_supported(source, target);
	return riscv64_aplic_source_target_supported(source, target);
}

bool hal_interrupt_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	if (source == NULL) return false;
	if (source->domain == 1u) return riscv64_plic_source_init(state, source, delivery);
	return riscv64_aplic_source_init(state, source, delivery);
}

bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (state->source.domain == 1u) return riscv64_plic_source_mask(state);
	return riscv64_aplic_source_mask(state);
}

bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (state->source.domain == 1u) return riscv64_plic_source_unmask(state);
	return riscv64_aplic_source_unmask(state);
}

bool hal_interrupt_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (state->source.domain == 1u) return riscv64_plic_source_deinit(state);
	return riscv64_aplic_source_deinit(state);
}

size_t hal_interrupt_message_range_count(void) {
	struct hal_interrupt_message_range range;
	return global_ready && riscv64_imsic_message_range_at(&range) ? 1u : 0u;
}
bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	return global_ready && index == 0u && riscv64_imsic_message_range_at(out_range);
}
bool hal_interrupt_message_resolve(uint64_t controller_register_address, uint32_t producer_id,
                                   struct hal_interrupt_message_context* out_context) {
	struct hal_interrupt_message_range range;
	if (controller_register_address != UINT64_MAX || producer_id != UINT32_MAX || out_context == NULL ||
	    !hal_interrupt_message_range_at(0u, &range))
		return false;
	*out_context = (struct hal_interrupt_message_context){.domain = range.domain};
	return true;
}
bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	return global_ready && riscv64_imsic_message_target_supported(domain, source, target);
}
bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	return global_ready && riscv64_imsic_message_init(state, request, out_message);
}
bool hal_interrupt_message_deinit(struct hal_interrupt_message_state* state) {
	return riscv64_imsic_message_deinit(state);
}

bool hal_interrupts_init_global(void) {
	global_ready = true;
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	uintptr_t                             entry;
	struct riscv64_exception_entry_state* entry_state;
	uint64_t                              sie;

	if (!global_ready || cpu == NULL || cpu->index >= 64u) return false;
	if (local_ready[cpu->index]) return true;

	entry       = (uintptr_t)exception_entry;
	entry_state = &riscv64_exception_entry_state[cpu->index];
	sie         = read_sie();
	sie &= ~(RISCV64_SIE_STIE | RISCV64_SIE_SEIE);
	sie |= RISCV64_SIE_SSIE;
	entry_state->kernel_stack_top = cpu->kernel_entry_stack_top;

	__asm__ volatile("csrw stvec, %0" : : "r"(entry) : "memory");
	write_sscratch((uint64_t)(uintptr_t)entry_state);
	clear_software_interrupt();
	write_sie(sie);
	irq_disable_local();

	local_ready[cpu->index] = true;
	riscv64_imsic_sync_local();
	riscv64_sync_external_interrupt_local();
	cpu_interrupts_set_ready(cpu, true);
	return true;
}

void riscv64_prepare_user_return(void) {
	struct cpu*                           cpu = cpu_current();
	struct riscv64_exception_entry_state* entry_state;

	sched_finish_context_switch();
	if (cpu == NULL || cpu->index >= 64u || cpu->kernel_entry_stack_top == 0u) hcf();

	entry_state                   = &riscv64_exception_entry_state[cpu->index];
	entry_state->kernel_stack_top = cpu->kernel_entry_stack_top;
	__asm__ volatile("" : : : "memory");
}

static const char* interrupt_name(uint64_t code) {
	switch (code) {
	case 1:
		return "Supervisor software interrupt";
	case 5:
		return "Supervisor timer interrupt";
	case 9:
		return "Supervisor external interrupt";
	default:
		return "Reserved/platform interrupt";
	}
}

static const char* exception_name(uint64_t code) {
	switch (code) {
	case 0:
		return "Instruction address misaligned";
	case 1:
		return "Instruction access fault";
	case 2:
		return "Illegal instruction";
	case 3:
		return "Breakpoint";
	case 4:
		return "Load address misaligned";
	case 5:
		return "Load access fault";
	case 6:
		return "Store/AMO address misaligned";
	case 7:
		return "Store/AMO access fault";
	case 8:
		return "Environment call from U-mode";
	case 9:
		return "Environment call from S-mode";
	case 12:
		return "Instruction page fault";
	case 13:
		return "Load page fault";
	case 15:
		return "Store/AMO page fault";
	default:
		return "Reserved/custom exception";
	}
}

static bool is_page_fault_exception(uint64_t code) {
	return code == 12u || code == 13u || code == 15u;
}

static bool was_user_mode(uint64_t sstatus) {
	return (sstatus & (1ull << 8)) == 0;
}

static memory_access_t page_fault_access(uint64_t code) {
	switch (code) {
	case 12:
		return MEMORY_ACCESS_EXEC;
	case 13:
		return MEMORY_ACCESS_READ;
	case 15:
		return MEMORY_ACCESS_WRITE;
	default:
		return 0u;
	}
}

static bool riscv64_exception_kind(uint64_t code, enum core_exception_kind* out_kind) {
	if (!out_kind) return false;
	switch (code) {
	case 0:
	case 4:
	case 6:
		*out_kind = CORE_EXCEPTION_ALIGNMENT;
		return true;
	case 1:
	case 5:
	case 7:
		*out_kind = code == 1 ? CORE_EXCEPTION_ACCESS_INSTRUCTION_ABORT : CORE_EXCEPTION_ACCESS_DATA_ABORT;
		return true;
	case 2:
		*out_kind = CORE_EXCEPTION_INSTRUCTION_ILLEGAL;
		return true;
	case 3:
		*out_kind = CORE_EXCEPTION_BREAKPOINT;
		return true;
	default:
		return false;
	}
}

static bool riscv64_handle_user_exception(uint64_t code, uint64_t sstatus) {
	enum core_exception_kind kind;

	if (!was_user_mode(sstatus)) return false;
	if (is_page_fault_exception(code)) return false;
	if (!riscv64_exception_kind(code, &kind)) return false;
	return core_handle_user_exception(kind);
}

void riscv64_maybe_preempt_on_interrupt_exit(void) {
	(void)sched_handle_interrupt_exit();
}

void handle_exception(struct exception_frame* frame) {
	bool     is_interrupt = (frame->scause >> 63) != 0;
	uint64_t code         = frame->scause & ~(1ull << 63);

	if (riscv64_handle_syscall(frame, is_interrupt, code)) return;
	if (!is_interrupt) cpu_enter_exception();
	if (is_interrupt && code == RISCV64_SCAUSE_SUPERVISOR_SOFTWARE) {
		clear_software_interrupt();
		riscv64_imsic_sync_local();
		riscv64_sync_external_interrupt_local();
		return;
	}
	if (clock_handle_irq(frame)) {
		if (!is_interrupt) cpu_leave_exception();
		return;
	}
	if (is_interrupt && code == RISCV64_SCAUSE_SUPERVISOR_EXTERNAL) {
		if (riscv64_imsic_handle_external_irq()) return;
		if (riscv64_aplic_handle_external_irq()) return;
		if (riscv64_plic_handle_external_irq()) return;
		riscv64_sync_external_interrupt_local();
		return;
	}

	if (!is_interrupt && is_page_fault_exception(code)) {
		cpu_leave_exception();
		if (address_space_handle_current_fault(frame->stval,
		                                       ADDRESS_SPACE_FAULT_UNCLASSIFIED,
		                                       page_fault_access(code),
		                                       was_user_mode(frame->sstatus))) {
			return;
		}
		cpu_enter_exception();
	}

	if (!is_interrupt && was_user_mode(frame->sstatus)) {
		cpu_leave_exception();
		if (riscv64_handle_user_exception(code, frame->sstatus)) return;
		cpu_enter_exception();
	}

	printf("kernel: riscv64 %s %llu (%s)\n",
	       is_interrupt ? "interrupt" : "exception",
	       code,
	       is_interrupt ? interrupt_name(code) : exception_name(code));
	printf("  scause=0x%016llx sepc=0x%016llx stval=0x%016llx sstatus=0x%016llx\n",
	       frame->scause,
	       frame->sepc,
	       frame->stval,
	       frame->sstatus);
	printf("  ra=0x%016llx sp=0x%016llx gp=0x%016llx tp=0x%016llx\n", frame->ra, frame->sp, frame->gp, frame->tp);
	printf("  a0=0x%016llx a1=0x%016llx a2=0x%016llx a3=0x%016llx\n", frame->a0, frame->a1, frame->a2, frame->a3);
	printf("  a4=0x%016llx a5=0x%016llx a6=0x%016llx a7=0x%016llx\n", frame->a4, frame->a5, frame->a6, frame->a7);

	hcf();
}
