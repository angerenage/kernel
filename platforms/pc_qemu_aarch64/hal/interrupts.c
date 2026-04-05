#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

static bool interrupts_initialized;

extern char exception_vectors[];

static inline void mask_irqs(void) {
	__asm__ volatile("msr daifset, #2" : : : "memory");
}

void hal_interrupts_init(void) {
	uintptr_t vectors;

	if (interrupts_initialized) return;

	vectors = (uintptr_t)exception_vectors;
	mask_irqs();

	__asm__ volatile("msr vbar_el1, %0\n\t"
	                 "isb"
	                 :
	                 : "r"(vectors)
	                 : "memory");

	interrupts_initialized = true;
	printf("kernel: aarch64 vectors installed\n");
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

void handle_exception(const struct exception_frame* frame) {
	uint64_t ec;
	uint64_t iss;

	if (clock_handle_irq(frame)) return;

	ec  = (frame->esr >> 26) & 0x3fu;
	iss = frame->esr & 0x01ffffffu;

	printf("kernel: aarch64 exception %s\n", vector_names[frame->vector & 0xfu]);
	printf("  esr=0x%016llx ec=0x%02llx (%s) far=0x%016llx\n", frame->esr, ec, ec_name(ec), frame->far);

	if (is_instruction_abort(ec) || is_data_abort(ec)) {
		uint64_t dfsc  = iss & 0x3fu;
		bool     s1ptw = ((iss >> 7) & 1u) != 0;
		bool     cm    = ((iss >> 8) & 1u) != 0;
		bool     ea    = ((iss >> 9) & 1u) != 0;
		bool     fnv   = ((iss >> 10) & 1u) != 0;

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
