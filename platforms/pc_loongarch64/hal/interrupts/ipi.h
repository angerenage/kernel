#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu;

enum loongarch64_ipi_vector {
	LOONGARCH64_IPI_VECTOR_WAKE = 0u,
	LOONGARCH64_IPI_VECTOR_TLB,
	LOONGARCH64_IPI_VECTOR_CACHE,
	LOONGARCH64_IPI_VECTOR_COUNT,
};

#define LOONGARCH64_IPI_STATUS_WAKE (1u << LOONGARCH64_IPI_VECTOR_WAKE)
#define LOONGARCH64_IPI_STATUS_TLB (1u << LOONGARCH64_IPI_VECTOR_TLB)
#define LOONGARCH64_IPI_STATUS_CACHE (1u << LOONGARCH64_IPI_VECTOR_CACHE)
#define LOONGARCH64_IPI_STATUS_MASK                                                                                    \
	(LOONGARCH64_IPI_STATUS_WAKE | LOONGARCH64_IPI_STATUS_TLB | LOONGARCH64_IPI_STATUS_CACHE)

/* Return true if the current CPU supports inter-processor interrupts. */
bool loongarch64_ipi_supported(void);

/* Initialize the local IPI for the current CPU. */
void loongarch64_ipi_init_local(void);

/* Complete a pending IPI request on the current CPU. Returns a bitmask of actions that were requested. */
uint32_t loongarch64_ipi_handle(void);

/* Service synchronization requests even while maskable IPIs are disabled. */
void loongarch64_ipi_poll_sync(void);

/* Send an IPI to the specified CPU. */
bool loongarch64_ipi_send(const struct cpu* cpu, enum loongarch64_ipi_vector vector);
