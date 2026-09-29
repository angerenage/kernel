#include "ipi.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>

#include "../cache.h"
#include "../paging.h"

#define LOONGARCH64_CSR_CPUID_MASK 0x7ffu

#define LOONGARCH64_IOCSR_FEATURES 0x8u
#define LOONGARCH64_IOCSR_FEATURE_CSRIPI (1ull << 4)
#define LOONGARCH64_IOCSR_IPI_STATUS 0x1000u
#define LOONGARCH64_IOCSR_IPI_ENABLE 0x1004u
#define LOONGARCH64_IOCSR_IPI_CLEAR 0x100cu
#define LOONGARCH64_IOCSR_IPI_SEND 0x1040u
#define LOONGARCH64_IOCSR_IPI_SEND_CPU_SHIFT 16u
#define LOONGARCH64_IOCSR_IPI_SEND_BLOCKING (1u << 31)

static inline uint64_t iocsr_read64(uint32_t address) {
	uint64_t value;

	__asm__ volatile("iocsrrd.d %0, %1" : "=r"(value) : "r"((uint64_t)address));
	return value;
}

static inline uint32_t iocsr_read32(uint32_t address) {
	uint64_t value;

	__asm__ volatile("iocsrrd.w %0, %1" : "=r"(value) : "r"((uint64_t)address));
	return (uint32_t)value;
}

static inline void iocsr_write32(uint32_t value, uint32_t address) {
	__asm__ volatile("iocsrwr.w %0, %1" : : "r"((uint64_t)value), "r"((uint64_t)address) : "memory");
}

bool loongarch64_ipi_supported(void) {
	return (iocsr_read64(LOONGARCH64_IOCSR_FEATURES) & LOONGARCH64_IOCSR_FEATURE_CSRIPI) != 0u;
}

void loongarch64_ipi_init_local(void) {
	if (!loongarch64_ipi_supported()) return;

	iocsr_write32(UINT32_MAX, LOONGARCH64_IOCSR_IPI_CLEAR);
	iocsr_write32(LOONGARCH64_IPI_STATUS_MASK, LOONGARCH64_IOCSR_IPI_ENABLE);
}

uint32_t loongarch64_ipi_handle(void) {
	uint32_t actions;

	if (!loongarch64_ipi_supported()) return 0u;
	actions = iocsr_read32(LOONGARCH64_IOCSR_IPI_STATUS) & LOONGARCH64_IPI_STATUS_MASK;
	if (actions != 0u) {
		iocsr_write32(actions, LOONGARCH64_IOCSR_IPI_CLEAR);
		/* Complete the clear before an acknowledgement can make the sender
		 * reuse the same action bit for its next request. */
		__asm__ volatile("dbar 0" : : : "memory");
	}
	return actions;
}

bool loongarch64_ipi_send(const struct cpu* cpu, enum loongarch64_ipi_vector vector) {
	uint32_t request;

	if (!loongarch64_ipi_supported() || cpu == NULL || cpu->arch_id > LOONGARCH64_CSR_CPUID_MASK ||
	    vector >= LOONGARCH64_IPI_VECTOR_COUNT)
		return false;

	request = LOONGARCH64_IOCSR_IPI_SEND_BLOCKING | ((uint32_t)cpu->arch_id << LOONGARCH64_IOCSR_IPI_SEND_CPU_SHIFT) |
	          (uint32_t)vector;
	__asm__ volatile("dbar 0" : : : "memory");
	iocsr_write32(request, LOONGARCH64_IOCSR_IPI_SEND);
	return true;
}

void loongarch64_ipi_poll_sync(void) {
	struct irq_state state;

	if (cpu_current() == NULL) return;
	/* Prevent an interrupt from acknowledging a newer generation between
	 * a poll's request load and acknowledgement store. Neither handler
	 * takes a lock or dispatches scheduler work. */
	state = irq_save_disable();
	(void)loongarch64_paging_handle_tlb_ipi();
	(void)loongarch64_cache_handle_sync_ipi();
	irq_restore(state);
}
