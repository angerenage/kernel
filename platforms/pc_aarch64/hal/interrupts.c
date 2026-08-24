#include <base/process.h>
#include <core/cpu.h>
#include <core/exception.h>
#include <core/sched.h>
#include <core/vm_space.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

static bool global_ready;
static bool local_ready[64];
extern char exception_vectors[];

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
	global_ready = true;
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	uintptr_t vectors;

	if (!global_ready || cpu == NULL || cpu->index >= 64u) return false;
	if (local_ready[cpu->index]) return aarch64_gic_init_local(cpu);

	vectors = (uintptr_t)exception_vectors;
	irq_disable_local();
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

static const char* const vector_names[16] = {
	"current EL, SP0, sync",
	"current EL, SP0, IRQ",
	"current EL, SP0, FIQ",
	"current EL, SP0, SError",
	"current EL, SPx, sync",
	"current EL, SPx, IRQ",
	"current EL, SPx, FIQ",
	"current EL, SPx, SError",
	"lower EL, AArch64, sync",
	"lower EL, AArch64, IRQ",
	"lower EL, AArch64, FIQ",
	"lower EL, AArch64, SError",
	"lower EL, AArch32, sync",
	"lower EL, AArch32, IRQ",
	"lower EL, AArch32, FIQ",
	"lower EL, AArch32, SError",
};

static const char* ec_name(uint64_t ec) {
	switch (ec) {
	case 0x00:
		return "Unknown";
	case 0x01:
		return "Trapped WFI/WFE";
	case 0x03:
		return "Trapped MCR/MRC";
	case 0x04:
		return "Trapped MCRR/MRRC";
	case 0x05:
		return "Trapped MCR/MRC coproc";
	case 0x06:
		return "Trapped LDC/STC";
	case 0x07:
		return "SIMD/FP access";
	case 0x0e:
		return "Illegal execution state";
	case 0x11:
		return "SVC AArch32";
	case 0x15:
		return "SVC AArch64";
	case 0x18:
		return "Trapped MSR/MRS/System";
	case 0x20:
		return "Instruction abort, lower EL";
	case 0x21:
		return "Instruction abort, same EL";
	case 0x22:
		return "PC alignment fault";
	case 0x24:
		return "Data abort, lower EL";
	case 0x25:
		return "Data abort, same EL";
	case 0x26:
		return "SP alignment fault";
	case 0x2c:
		return "Trapped FP exception";
	case 0x2f:
		return "SError";
	default:
		return "Reserved/implementation defined";
	}
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

static enum vmm_fault_kind abort_fault_kind(uint64_t dfsc) {
	if (is_translation_fault(dfsc)) return VMM_FAULT_NOT_PRESENT;
	if (dfsc >= 0x08 && dfsc <= 0x0fu) return VMM_FAULT_PROTECTION;
	return VMM_FAULT_INVALID;
}

static enum vmm_fault_access abort_fault_access(uint64_t ec, uint64_t iss) {
	if (is_instruction_abort(ec)) return VMM_FAULT_ACCESS_EXEC;
	if (!is_data_abort(ec)) return VMM_FAULT_ACCESS_UNKNOWN;
	return ((iss >> 6) & 1u) != 0 ? VMM_FAULT_ACCESS_WRITE : VMM_FAULT_ACCESS_READ;
}

static const char* abort_dfsc_name(uint64_t dfsc) {
	switch (dfsc) {
	case 0x00:
		return "Address size fault, level 0";
	case 0x01:
		return "Address size fault, level 1";
	case 0x02:
		return "Address size fault, level 2";
	case 0x03:
		return "Address size fault, level 3";
	case 0x04:
		return "Translation fault, level 0";
	case 0x05:
		return "Translation fault, level 1";
	case 0x06:
		return "Translation fault, level 2";
	case 0x07:
		return "Translation fault, level 3";
	case 0x08:
		return "Access flag fault, level 0";
	case 0x09:
		return "Access flag fault, level 1";
	case 0x0a:
		return "Access flag fault, level 2";
	case 0x0b:
		return "Access flag fault, level 3";
	case 0x0c:
		return "Permission fault, level 0";
	case 0x0d:
		return "Permission fault, level 1";
	case 0x0e:
		return "Permission fault, level 2";
	case 0x0f:
		return "Permission fault, level 3";
	case 0x10:
		return "Synchronous external abort";
	case 0x11:
		return "Synchronous tag check fault";
	case 0x12:
		return "Synchronous external abort on TT walk, level 0";
	case 0x13:
		return "Synchronous external abort on TT walk, level 1";
	case 0x14:
		return "Synchronous external abort on TT walk, level 2";
	case 0x15:
		return "Synchronous external abort on TT walk, level 3";
	case 0x18:
		return "Synchronous parity/ECC error";
	case 0x1c:
		return "Synchronous parity/ECC error on TT walk, level 0";
	case 0x1d:
		return "Synchronous parity/ECC error on TT walk, level 1";
	case 0x1e:
		return "Synchronous parity/ECC error on TT walk, level 2";
	case 0x1f:
		return "Synchronous parity/ECC error on TT walk, level 3";
	case 0x21:
		return "Alignment fault";
	case 0x28:
		return "Granule protection fault, level 0";
	case 0x29:
		return "Granule protection fault, level 1";
	case 0x2a:
		return "Granule protection fault, level 2";
	case 0x2b:
		return "Granule protection fault, level 3";
	case 0x30:
		return "TLB conflict abort";
	case 0x31:
		return "Unsupported atomic hardware update";
	default:
		return "Reserved/implementation defined";
	}
}

static const char* abort_target_el(uint64_t ec) {
	switch (ec) {
	case 0x20:
	case 0x24:
		return "lower EL";
	case 0x21:
	case 0x25:
		return "same EL";
	default:
		return "unknown EL";
	}
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

	if (aarch64_handle_syscall(frame, ec)) return;
	if (!is_irq) cpu_enter_exception();
	if (clock_handle_irq(frame)) {
		if (!is_irq) cpu_leave_exception();
		return;
	}

	uint64_t iss = frame->esr & 0x01ffffffu;

	uint64_t dfsc  = iss & 0x3fu;
	bool     s1ptw = ((iss >> 7) & 1u) != 0;
	bool     cm    = ((iss >> 8) & 1u) != 0;
	bool     ea    = ((iss >> 9) & 1u) != 0;
	bool     fnv   = ((iss >> 10) & 1u) != 0;

	if ((is_instruction_abort(ec) || is_data_abort(ec)) && is_page_fault_abort(dfsc)) {
		if (!is_irq) cpu_leave_exception();
		if (vm_handle_current_page_fault(fnv ? 0u : frame->far,
		                                 fnv ? VMM_FAULT_INVALID : abort_fault_kind(dfsc),
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

	printf("kernel: aarch64 exception %s\n", vector_names[frame->vector & 0xfu]);
	printf("  esr=0x%016llx ec=0x%02llx (%s) far=0x%016llx\n", frame->esr, ec, ec_name(ec), frame->far);

	if (is_instruction_abort(ec) || is_data_abort(ec)) {
		printf("  abort=%s target=%s dfsc=0x%02llx (%s)\n",
		       is_data_abort(ec) ? "data" : "instruction",
		       abort_target_el(ec),
		       dfsc,
		       abort_dfsc_name(dfsc));

		if (is_data_abort(ec)) {
			bool write = ((iss >> 6) & 1u) != 0;
			printf("  access=%s s1ptw=%s cache_maint=%s ext_abort=%s far_valid=%s\n",
			       write ? "write" : "read",
			       s1ptw ? "yes" : "no",
			       cm ? "yes" : "no",
			       ea ? "yes" : "no",
			       fnv ? "no" : "yes");
		}
		else {
			printf(
				"  s1ptw=%s ext_abort=%s far_valid=%s\n", s1ptw ? "yes" : "no", ea ? "yes" : "no", fnv ? "no" : "yes");
		}
	}

	printf("  elr=0x%016llx spsr=0x%016llx\n", frame->elr, frame->spsr);
	printf("  x0 =0x%016llx x1 =0x%016llx x2 =0x%016llx x3 =0x%016llx\n",
	       frame->x[0],
	       frame->x[1],
	       frame->x[2],
	       frame->x[3]);
	printf("  x4 =0x%016llx x5 =0x%016llx x6 =0x%016llx x7 =0x%016llx\n",
	       frame->x[4],
	       frame->x[5],
	       frame->x[6],
	       frame->x[7]);
	printf("  x29=0x%016llx x30=0x%016llx\n", frame->x[29], frame->x[30]);

	hcf();
}
