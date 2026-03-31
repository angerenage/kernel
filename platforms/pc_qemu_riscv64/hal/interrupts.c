#include <hal/hcf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

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

void handle_exception(const struct exception_frame* frame) {
	bool     is_interrupt;
	uint64_t code;

	if (clock_handle_irq(frame)) return;

	is_interrupt = (frame->scause >> 63) != 0;
	code         = frame->scause & ~(1ull << 63);

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
