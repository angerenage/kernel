#include <core/vmm.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

#define LOONGARCH64_CSR_ECFG 0x4u
#define LOONGARCH64_CSR_EENTRY 0xcu
#define LOONGARCH64_CSR_SAVE1 0x31u
#define LOONGARCH64_CSR_TLBRENTRY 0x88u
#define LOONGARCH64_CSR_MERRENTRY 0x94u
#define LOONGARCH64_EXCEPTION_STACK_SIZE 0x4000u

static bool interrupts_initialized;
_Alignas(16) uint8_t loongarch64_exception_stack[LOONGARCH64_EXCEPTION_STACK_SIZE];
uintptr_t loongarch64_exception_stack_top =
	(uintptr_t)(loongarch64_exception_stack + sizeof(loongarch64_exception_stack));
uintptr_t loongarch64_exception_stack_bottom = (uintptr_t)loongarch64_exception_stack;

extern void exception_entry(void);
extern void tlb_refill_entry(void);
extern void machine_error_entry(void);

static inline uintptr_t kernel_virt_to_phys(const void* ptr) {
	if (exec_addr_req.response == NULL) {
		return 0;
	}

	return (uintptr_t)(exec_addr_req.response->physical_base +
	                   ((uint64_t)(uintptr_t)ptr - exec_addr_req.response->virtual_base));
}

static inline void csrwr(uint64_t value, unsigned csr) {
	switch (csr) {
	case LOONGARCH64_CSR_ECFG:
		__asm__ volatile("csrwr %0, 0x4" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_EENTRY:
		__asm__ volatile("csrwr %0, 0xc" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_SAVE1:
		__asm__ volatile("csrwr %0, 0x31" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_TLBRENTRY:
		__asm__ volatile("csrwr %0, 0x88" : : "r"(value) : "memory");
		break;
	case LOONGARCH64_CSR_MERRENTRY:
		__asm__ volatile("csrwr %0, 0x94" : : "r"(value) : "memory");
		break;
	default:
		break;
	}
}

bool hal_interrupts_init(void) {
	uintptr_t trap_entry;
	uintptr_t tlbr_entry;
	uintptr_t merr_entry;

	if (interrupts_initialized) return true;

	trap_entry = (uintptr_t)exception_entry;
	tlbr_entry = kernel_virt_to_phys((const void*)tlb_refill_entry);
	merr_entry = kernel_virt_to_phys((const void*)machine_error_entry);

	if (exec_addr_req.response == NULL || tlbr_entry == 0 || merr_entry == 0) {
		printf("kernel: loongarch64 kernel address response missing for trap setup\n");
		return false;
	}

	csrwr(0u, LOONGARCH64_CSR_ECFG);
	csrwr(trap_entry, LOONGARCH64_CSR_EENTRY);
	csrwr(loongarch64_exception_stack_top, LOONGARCH64_CSR_SAVE1);
	csrwr(tlbr_entry, LOONGARCH64_CSR_TLBRENTRY);
	csrwr(merr_entry, LOONGARCH64_CSR_MERRENTRY);

	interrupts_initialized = true;
	printf("kernel: loongarch64 trap entries installed (eentry=%p tlbrentry=%p merrentry=%p exc_sp=%p)\n",
	       (void*)trap_entry,
	       (void*)tlbr_entry,
	       (void*)merr_entry,
	       (void*)loongarch64_exception_stack_top);
	return true;
}

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

static bool is_page_invalid_exception(uint64_t ecode) {
	return ecode >= 0x1u && ecode <= 0x3u;
}

void handle_exception(const struct exception_frame* frame) {
	uint64_t is_pending;
	uint64_t ecode;
	uint64_t esubcode;

	if (clock_handle_irq(frame)) return;

	is_pending = frame->estat & 0x1fffu;
	ecode      = (frame->estat >> 16) & 0x3fu;
	esubcode   = (frame->estat >> 22) & 0x1ffu;

	if (is_page_invalid_exception(ecode) && vmm_resolve_page_fault(frame->badv)) return;

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
