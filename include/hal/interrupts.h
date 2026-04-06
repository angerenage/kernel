#pragma once

#include <core/cpu.h>
#include <stdbool.h>

struct irq_state {
	bool enabled;
};

bool hal_interrupts_init_global(void);
bool hal_interrupts_init_local(struct cpu* cpu);

#if defined(PLATFORM_PC_X86_64)
static inline bool irq_enabled(void) {
	uint64_t flags;

	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
	return (flags & (1ull << 9)) != 0;
}

static inline void irq_disable_local(void) {
	__asm__ volatile("cli" : : : "memory");
}

static inline void irq_enable_local(void) {
	__asm__ volatile("sti" : : : "memory");
}
#elif defined(PLATFORM_PC_AARCH64)
static inline bool irq_enabled(void) {
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	return (daif & (1u << 7)) == 0;
}

static inline void irq_disable_local(void) {
	__asm__ volatile("msr daifset, #2" : : : "memory");
}

static inline void irq_enable_local(void) {
	__asm__ volatile("msr daifclr, #2" : : : "memory");
}
#elif defined(PLATFORM_PC_RISCV64)
static inline bool irq_enabled(void) {
	uint64_t sstatus;

	__asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
	return (sstatus & (1ull << 1)) != 0;
}

static inline void irq_disable_local(void) {
	__asm__ volatile("csrc sstatus, %0" : : "r"(1ull << 1) : "memory");
}

static inline void irq_enable_local(void) {
	__asm__ volatile("csrs sstatus, %0" : : "r"(1ull << 1) : "memory");
}
#elif defined(PLATFORM_PC_LOONGARCH64)
static inline bool irq_enabled(void) {
	uint64_t crmd;

	__asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
	return (crmd & (1u << 2)) != 0;
}

static inline void irq_disable_local(void) {
	uint64_t crmd;

	__asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
	crmd &= ~(1u << 2);
	__asm__ volatile("csrwr %0, 0x0" : : "r"(crmd) : "memory");
}

static inline void irq_enable_local(void) {
	uint64_t crmd;

	__asm__ volatile("csrrd %0, 0x0" : "=r"(crmd));
	crmd |= 1u << 2;
	__asm__ volatile("csrwr %0, 0x0" : : "r"(crmd) : "memory");
}
#else
static _Thread_local bool hosted_irq_enabled = true;

static inline bool irq_enabled(void) {
	return hosted_irq_enabled;
}

static inline void irq_disable_local(void) {
	hosted_irq_enabled = false;
}

static inline void irq_enable_local(void) {
	hosted_irq_enabled = true;
}
#endif

static inline struct irq_state irq_save_disable(void) {
	struct irq_state state = {
		.enabled = irq_enabled(),
	};
	struct cpu* cpu = cpu_current();

	irq_disable_local();
	if (cpu != NULL) cpu->irq_disable_depth++;
	return state;
}

static inline void irq_restore(struct irq_state state) {
	struct cpu* cpu = cpu_current();

	if (cpu != NULL && cpu->irq_disable_depth != 0u) cpu->irq_disable_depth--;
	if (state.enabled) irq_enable_local();
}

static inline bool irq_in_exception(void) {
	return cpu_irq_in_exception();
}
