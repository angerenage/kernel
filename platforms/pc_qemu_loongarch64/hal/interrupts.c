#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <kernel/requests.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LOONGARCH64_CSR_ECFG 0x4u
#define LOONGARCH64_CSR_EENTRY 0xcu
#define LOONGARCH64_CSR_TLBRENTRY 0x88u
#define LOONGARCH64_CSR_MERRENTRY 0x94u

struct loongarch64_exception_frame {
	uint64_t gpr[32];
	uint64_t estat;
	uint64_t era;
	uint64_t badv;
	uint64_t reserved;
};

extern void loongarch64_exception_entry(void);
extern void loongarch64_tlb_refill_entry(void);
extern void loongarch64_machine_error_entry(void);

static uintptr_t loongarch64_kernel_virt_to_phys(const void* ptr) {
	if (exec_addr_req.response == NULL) {
		return 0;
	}

	return (uintptr_t)(exec_addr_req.response->physical_base +
	                   ((uint64_t)(uintptr_t)ptr - exec_addr_req.response->virtual_base));
}

static const char* loongarch64_ecode_name(uint64_t ecode, uint64_t esubcode) {
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

void hal_interrupts_init(void) {
	uintptr_t exception_entry = (uintptr_t)loongarch64_exception_entry;
	uintptr_t tlbr_entry      = loongarch64_kernel_virt_to_phys((const void*)loongarch64_tlb_refill_entry);
	uintptr_t merr_entry      = loongarch64_kernel_virt_to_phys((const void*)loongarch64_machine_error_entry);
	uintptr_t zero            = 0;

	if (exec_addr_req.response == NULL || tlbr_entry == 0 || merr_entry == 0) {
		printf("kernel: loongarch64 kernel address response missing for trap setup\n");
		hcf();
	}

	__asm__ volatile("csrwr %0, %1" : : "r"(zero), "i"(LOONGARCH64_CSR_ECFG) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(exception_entry), "i"(LOONGARCH64_CSR_EENTRY) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(tlbr_entry), "i"(LOONGARCH64_CSR_TLBRENTRY) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(merr_entry), "i"(LOONGARCH64_CSR_MERRENTRY) : "memory");

	printf("kernel: loongarch64 trap entries installed (eentry=%p tlbrentry=%p merrentry=%p)\n",
	       (void*)exception_entry,
	       (void*)tlbr_entry,
	       (void*)merr_entry);
}

__attribute__((noreturn))
void loongarch64_handle_exception(const struct loongarch64_exception_frame* frame) {
	uint64_t is_pending = frame->estat & 0x1fffu;
	uint64_t ecode      = (frame->estat >> 16) & 0x3fu;
	uint64_t esubcode   = (frame->estat >> 22) & 0x1ffu;

	printf("kernel: loongarch64 exception ecode=0x%02llx esubcode=0x%03llx (%s)\n",
	       ecode,
	       esubcode,
	       loongarch64_ecode_name(ecode, esubcode));
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
