#include <hal/hcf.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

static const char* ecode_name(uint64_t ecode, uint64_t esubcode) {
	(void)esubcode;

	switch (ecode) {
	case 0x0:
		return "Interrupt";
	case 0x1:
		return "Page invalid load";
	case 0x2:
		return "Page invalid store";
	case 0x3:
		return "Page invalid fetch";
	case 0x4:
		return "Page modified";
	case 0x5:
		return "Page non-readable";
	case 0x6:
		return "Page non-executable";
	case 0x7:
		return "Page privilege illegal";
	case 0x8:
		return esubcode == 0 ? "Address error fetch" : "Address error memory";
	case 0x9:
		return "Address alignment";
	case 0xa:
		return "Bound check";
	case 0xb:
		return "System call";
	case 0xc:
		return "Breakpoint";
	case 0xd:
		return "Instruction not defined";
	case 0xe:
		return "Instruction privilege error";
	case 0xf:
		return "FP disabled";
	case 0x10:
		return "128-bit SIMD disabled";
	case 0x11:
		return "256-bit SIMD disabled";
	case 0x12:
		return esubcode == 0 ? "Floating-point error" : "Vector floating-point error";
	case 0x13:
		return esubcode == 0 ? "Fetch watchpoint" : "Memory watchpoint";
	case 0x14:
		return "Binary translation disabled";
	case 0x15:
		return "Binary translation exception";
	case 0x16:
		return "Guest sensitive privilege resource";
	case 0x17:
		return "Hypervisor call";
	case 0x18:
		return esubcode == 0 ? "Guest CSR software change" : "Guest CSR hardware change";
	default:
		return "Reserved";
	}
}

void handle_exception(const struct exception_frame* frame) {
	uint64_t is_pending;
	uint64_t ecode;
	uint64_t esubcode;

	if (clock_handle_irq(frame)) return;

	is_pending = frame->estat & 0x1fffu;
	ecode      = (frame->estat >> 16) & 0x3fu;
	esubcode   = (frame->estat >> 22) & 0x1ffu;

	printf("kernel: loongarch64 exception ecode=0x%02llx esubcode=0x%03llx (%s)\n",
	       ecode,
	       esubcode,
	       ecode_name(ecode, esubcode));
	printf("  estat=0x%016llx era=0x%016llx badv=0x%016llx is=0x%04llx\n",
	       frame->estat,
	       frame->era,
	       frame->badv,
	       is_pending);
	printf("  ra=0x%016llx sp=0x%016llx tp=0x%016llx a0=0x%016llx\n",
	       frame->gpr[1],
	       frame->gpr[3],
	       frame->gpr[2],
	       frame->gpr[4]);
	printf("  a1=0x%016llx a2=0x%016llx a3=0x%016llx a4=0x%016llx\n",
	       frame->gpr[5],
	       frame->gpr[6],
	       frame->gpr[7],
	       frame->gpr[8]);

	hcf();
}
