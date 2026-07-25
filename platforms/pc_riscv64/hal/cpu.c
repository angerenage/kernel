#include <core/cpu.h>
#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RISCV64_SBI_EID_BASE 0x10ul
#define RISCV64_SBI_FID_PROBE_EXTENSION 3ul
#define RISCV64_SBI_EID_IPI 0x735049ul
#define RISCV64_SBI_FID_SEND_IPI 0ul

struct riscv64_sbi_ret {
	long error;
	long value;
};

enum {
	RISCV64_THREAD_STACK_ALIGNMENT = 16u,
	RISCV64_THREAD_CTX_S0          = 0,
	RISCV64_THREAD_CTX_S1,
	RISCV64_THREAD_CTX_S2,
	RISCV64_THREAD_CTX_S3,
	RISCV64_THREAD_CTX_S4,
	RISCV64_THREAD_CTX_S5,
	RISCV64_THREAD_CTX_S6,
	RISCV64_THREAD_CTX_S7,
	RISCV64_THREAD_CTX_S8,
	RISCV64_THREAD_CTX_S9,
	RISCV64_THREAD_CTX_S10,
	RISCV64_THREAD_CTX_S11,
};

extern void riscv64_thread_context_switch(struct thread_context* current, const struct thread_context* next);
extern void riscv64_thread_entry(void);

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > RISCV64_THREAD_CTX_S11, "riscv64 thread spill area is too small");

static struct riscv64_sbi_ret riscv64_sbi_call2(unsigned long arg0, unsigned long arg1, unsigned long fid,
                                                unsigned long eid) {
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a7 asm("a7") = eid;

	__asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory", "a2", "a3", "a4", "a5");
	return (struct riscv64_sbi_ret){
		.error = (long)a0,
		.value = (long)a1,
	};
}

uint64_t hal_cpu_boot_arch_id(void) {
	return 0u;
}

void* hal_cpu_local_current(void) {
	uintptr_t value;

	__asm__ volatile("mv %0, tp" : "=r"(value));
	return (void*)value;
}

void hal_cpu_local_bind(void* ptr) {
	__asm__ volatile("mv tp, %0" : : "r"((uintptr_t)ptr) : "memory");
}

bool hal_cpu_thread_context_init(struct thread_context* context, uintptr_t stack_base, uintptr_t stack_top,
                                 uintptr_t entry_pc, uintptr_t entry_arg) {
	uintptr_t aligned_top;

	if (context == NULL || entry_pc == 0u || stack_top <= stack_base) return false;

	/*
	 * riscv64_thread_context_switch restores s0-s11 and jumps into
	 * riscv64_thread_entry, which moves s1 -> a0 and jumps to s0.
	 * The RISC-V psABI requires sp to stay 16-byte aligned.
	 */
	aligned_top = stack_top & ~((uintptr_t)RISCV64_THREAD_STACK_ALIGNMENT - 1u);
	if (aligned_top <= stack_base) return false;

	*context = (struct thread_context){
		.instruction_pointer = (uintptr_t)riscv64_thread_entry,
		.stack_pointer       = aligned_top,
	};
	context->spill[RISCV64_THREAD_CTX_S0] = entry_pc;
	context->spill[RISCV64_THREAD_CTX_S1] = entry_arg;
	return true;
}

void hal_cpu_context_switch(struct thread_context* current, const struct thread_context* next) {
	if (current == NULL || next == NULL) return;
	riscv64_thread_context_switch(current, next);
}

bool hal_cpu_prepare_smp(void) {
	struct riscv64_sbi_ret ret =
		riscv64_sbi_call2(RISCV64_SBI_EID_IPI, 0u, RISCV64_SBI_FID_PROBE_EXTENSION, RISCV64_SBI_EID_BASE);

	return ret.error == 0 && ret.value != 0;
}

void hal_cpu_park(void) {
	__asm__ volatile("wfi" : : : "memory");
}

void hal_cpu_kick(const struct cpu* cpu) {
	if (cpu == NULL) return;

	/* A one-bit mask based at the target hart ID selects exactly that hart. */
	(void)riscv64_sbi_call2(1ul, (unsigned long)cpu->arch_id, RISCV64_SBI_FID_SEND_IPI, RISCV64_SBI_EID_IPI);
}
