#include <base/process.h>
#include <core/address_space.h>
#include <core/cpu.h>
#include <core/exception.h>
#include <core/sched.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <kernel/crash.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../cache.h"
#include "../syscall.h"
#include "frame.h"
#include "gic.h"

static bool global_ready;
static bool local_ready[64];
extern char exception_vectors[];
extern void aarch64_select_kernel_stack(void);

static bool kernel_runs_at_el1(void) {
	uint64_t current_el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
	return ((current_el >> 2u) & 0x3u) == 1u;
}

bool irq_enabled(void) {
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	return (daif & (1u << 7)) == 0;
}

void irq_disable_local(void) {
	__asm__ volatile("msr daifset, #2" : : : "memory");
}

void irq_enable_local(void) {
	__asm__ volatile("msr daifclr, #2" : : : "memory");
}

bool hal_interrupts_init_global(void) {
	if (global_ready) return true;
	/* Limine's UEFI path enters the kernel at Non-secure EL1. The GIC
	 * backend selects the interrupt group for its Security Extensions model. */
	if (!kernel_runs_at_el1()) return false;
	if (!aarch64_gic_init_global()) return false;
	global_ready = true;
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	uintptr_t vectors;

	if (!global_ready || cpu == NULL || cpu->index >= 64u) return false;
	if (local_ready[cpu->index]) return aarch64_gic_init_local(cpu);

	vectors = (uintptr_t)exception_vectors;
	irq_disable_local();
	aarch64_select_kernel_stack();
	__asm__ volatile("msr vbar_el1, %0\n\t"
	                 "isb"
	                 :
	                 : "r"(vectors)
	                 : "memory");

	if (!aarch64_gic_init_local(cpu)) return false;
	local_ready[cpu->index] = true;
	cpu_interrupts_set_ready(cpu, true);
	return true;
}

static bool is_instruction_abort(uint64_t ec) {
	return ec == 0x20 || ec == 0x21;
}

static bool is_data_abort(uint64_t ec) {
	return ec == 0x24 || ec == 0x25;
}

static bool is_translation_fault(uint64_t dfsc) {
	return dfsc >= 0x04 && dfsc <= 0x07;
}

static bool is_abort_from_lower_el(uint64_t ec) {
	return ec == 0x20 || ec == 0x24;
}

static bool is_page_fault_abort(uint64_t dfsc) {
	return dfsc >= 0x04 && dfsc <= 0x0f;
}

static enum address_space_fault_kind abort_fault_kind(uint64_t dfsc) {
	if (is_translation_fault(dfsc)) return ADDRESS_SPACE_FAULT_NOT_PRESENT;
	if (dfsc >= 0x08 && dfsc <= 0x0fu) return ADDRESS_SPACE_FAULT_PROTECTION;
	return ADDRESS_SPACE_FAULT_INVALID;
}

static memory_access_t abort_fault_access(uint64_t ec, uint64_t iss) {
	if (is_instruction_abort(ec)) return MEMORY_ACCESS_EXEC;
	if (!is_data_abort(ec)) return 0u;
	return ((iss >> 6) & 1u) != 0 ? MEMORY_ACCESS_WRITE : MEMORY_ACCESS_READ;
}

static bool aarch64_exception_from_lower_el(const struct exception_frame* frame) {
	return (frame->vector & 0x8u) != 0u;
}

static bool aarch64_exception_kind(uint64_t ec, uint64_t dfsc, enum core_exception_kind* out_kind) {
	if (!out_kind) return false;
	switch (ec) {
	case 0x0e:
	case 0x01:
	case 0x03:
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x18:
		*out_kind = CORE_EXCEPTION_INSTRUCTION_ILLEGAL;
		return true;
	case 0x20:
	case 0x21:
		if (is_page_fault_abort(dfsc)) return false;
		*out_kind = CORE_EXCEPTION_ACCESS_INSTRUCTION_ABORT;
		return true;
	case 0x22:
	case 0x26:
		*out_kind = CORE_EXCEPTION_ALIGNMENT;
		return true;
	case 0x24:
	case 0x25:
		if (dfsc == 0x21u) {
			*out_kind = CORE_EXCEPTION_ALIGNMENT;
			return true;
		}
		if (is_page_fault_abort(dfsc)) return false;
		*out_kind = CORE_EXCEPTION_ACCESS_DATA_ABORT;
		return true;
	case 0x2c:
		*out_kind = CORE_EXCEPTION_FLOATING_POINT;
		return true;
	case 0x2f:
		*out_kind = CORE_EXCEPTION_BUS_ERROR;
		return true;
	default:
		return false;
	}
}

static bool aarch64_handle_user_exception(uint64_t ec, uint64_t dfsc, const struct exception_frame* frame) {
	enum core_exception_kind kind;

	if (!aarch64_exception_from_lower_el(frame)) return false;
	if (!aarch64_exception_kind(ec, dfsc, &kind)) return false;
	return core_handle_user_exception(kind);
}

void aarch64_maybe_preempt_on_interrupt_exit(void) {
	(void)sched_handle_interrupt_exit();
}

void aarch64_prepare_user_return(void) {
	sched_finish_context_switch();
}

void handle_exception(struct exception_frame* frame) {
	bool     is_irq = (frame->vector & 0x3u) == 1u;
	uint64_t ec     = (frame->esr >> 26) & 0x3fu;

	/* IRQ cache acknowledgements belong after the GIC has consumed the
	 * interrupt, so a subsequent generation cannot lose its SGI by coalescing
	 * with the still-pending one. Synchronous exceptions may still poll. */
	if (!is_irq) aarch64_cache_poll_sync();
	if (aarch64_handle_syscall(frame, ec)) return;
	if (!is_irq) cpu_enter_exception();
	if (aarch64_gic_handle_irq(frame)) {
		if (!is_irq) cpu_leave_exception();
		return;
	}

	uint64_t iss = frame->esr & 0x01ffffffu;

	uint64_t dfsc = iss & 0x3fu;
	bool     fnv  = ((iss >> 10) & 1u) != 0;

	if ((is_instruction_abort(ec) || is_data_abort(ec)) && is_page_fault_abort(dfsc)) {
		if (!is_irq) cpu_leave_exception();
		if (address_space_handle_current_fault(fnv ? 0u : frame->far,
		                                       fnv ? ADDRESS_SPACE_FAULT_INVALID : abort_fault_kind(dfsc),
		                                       abort_fault_access(ec, iss),
		                                       is_abort_from_lower_el(ec))) {
			return;
		}
		if (!is_irq) cpu_enter_exception();
	}

	if (!is_irq && aarch64_exception_from_lower_el(frame)) {
		cpu_leave_exception();
		if (aarch64_handle_user_exception(ec, dfsc, frame)) return;
		cpu_enter_exception();
	}

	struct kernel_crash_record* record = kernel_crash_begin(
		KERNEL_CRASH_ARCH_AARCH64, is_irq ? KERNEL_CRASH_CAUSE_UNEXPECTED_INTERRUPT : KERNEL_CRASH_CAUSE_EXCEPTION);
	struct kernel_crash_panic panic;

	if (kernel_crash_take_panic(&panic)) {
		record->cause        = KERNEL_CRASH_CAUSE_PANIC;
		record->panic_reason = panic.reason;
		record->panic_detail = panic.detail;
	}
	record->exception_code      = ec;
	record->exception_subcode   = iss;
	record->error_code          = frame->esr;
	record->fault_address       = frame->far;
	record->instruction_pointer = frame->elr;
	record->stack_pointer =
		aarch64_exception_from_lower_el(frame) ? frame->sp_el0 : (uintptr_t)frame + sizeof(*frame) + 16u;
	record->frame_pointer  = frame->x[29];
	record->status         = frame->spsr;
	record->register_count = 31u;
	for (uint32_t i = 0u; i < record->register_count; i++) record->registers[i] = frame->x[i];
	kernel_crash_finish(record);
}
