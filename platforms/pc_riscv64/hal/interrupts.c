#include <core/cpu.h>
#include <core/vmm.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

#define RISCV64_EXCEPTION_STACK_SIZE 0x4000u

extern void exception_entry(void);

static bool global_ready;
static bool local_ready[64];
_Alignas(16) uint8_t riscv64_exception_stack[64][RISCV64_EXCEPTION_STACK_SIZE];
uintptr_t riscv64_exception_stack_top;
uintptr_t riscv64_exception_stack_bottom;
struct riscv64_exception_entry_state {
	uintptr_t old_sp;
	uintptr_t saved_t1;
	uintptr_t saved_t2;
	uintptr_t scratch;
} riscv64_exception_entry_state;

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

bool hal_interrupts_init_global(void) {
	global_ready = true;
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	uintptr_t entry;
	uint64_t  sie;

	if (!global_ready || cpu == NULL || cpu->index >= 64u) return false;
	if (local_ready[cpu->index]) return true;

	entry = (uintptr_t)exception_entry;
	sie   = read_sie();
	sie &= ~(1ull << 5);
	riscv64_exception_stack_bottom = (uintptr_t)riscv64_exception_stack[cpu->index];
	riscv64_exception_stack_top    = riscv64_exception_stack_bottom + RISCV64_EXCEPTION_STACK_SIZE;

	__asm__ volatile("csrw stvec, %0" : : "r"(entry) : "memory");
	write_sscratch((uint64_t)(uintptr_t)&riscv64_exception_entry_state);
	write_sie(sie);
	irq_disable_local();

	local_ready[cpu->index] = true;
	cpu_interrupts_set_ready(cpu, true);
	if (cpu->role == CPU_ROLE_BSP) {
		printf("kernel: riscv64 local trap vector installed on cpu%zu (exc_sp=0x%016llx)\n",
		       cpu->index,
		       (unsigned long long)riscv64_exception_stack_top);
	}
	return true;
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

void handle_exception(const struct exception_frame* frame) {
	bool     is_interrupt;
	uint64_t code;

	cpu_enter_exception();
	if (clock_handle_irq(frame)) {
		cpu_leave_exception();
		return;
	}

	is_interrupt = (frame->scause >> 63) != 0;
	code         = frame->scause & ~(1ull << 63);

	if (!is_interrupt && is_page_fault_exception(code) && vmm_resolve_page_fault(frame->stval)) {
		cpu_leave_exception();
		return;
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
