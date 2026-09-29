#include "cache.h"

#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <stdint.h>

#include "interrupts/ipi.h"

#define LOONGARCH_CACHE_MAX_CPUS 64u

struct loongarch_cache_sync_request {
	size_t   source_index;
	uint64_t generation;
};

static struct loongarch_cache_sync_request cache_request;
static uint64_t                            cache_ack[LOONGARCH_CACHE_MAX_CPUS];
static struct spinlock                     cache_sync_lock =
	SPINLOCK_INIT_CLASS("cache_sync_lock", SPINLOCK_ORDER_NONE, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

bool hal_cache_sync_for_device(void* address, size_t size) {
	if (size == 0u) return true;
	if (address == NULL) return false;
	(void)address;
	/* DBAR orders accesses but is not a substitute for data-cache maintenance on non-coherent DMA. */
	__asm__ volatile("dbar 0" : : : "memory");
	return false;
}

bool hal_cache_sync_for_cpu(void* address, size_t size) {
	if (size == 0u) return true;
	if (address == NULL) return false;
	(void)address;
	/* Keep this unsupported until cache geometry and CACOP-based range maintenance are explicit. */
	__asm__ volatile("dbar 0" : : : "memory");
	return false;
}

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	__asm__ volatile("dbar 0\n\tibar 0" : : : "memory");
}

bool loongarch64_cache_handle_sync_ipi(void) {
	struct cpu* cpu = cpu_current();
	uint64_t    generation;

	if (cpu == NULL || cpu->index >= LOONGARCH_CACHE_MAX_CPUS) hcf();
	generation = __atomic_load_n(&cache_request.generation, __ATOMIC_ACQUIRE);
	if (generation == 0u || cpu->index == cache_request.source_index ||
	    __atomic_load_n(&cache_ack[cpu->index], __ATOMIC_ACQUIRE) == generation)
		return false;
	hal_cache_sync_executable_range(NULL, 0u);
	__atomic_store_n(&cache_ack[cpu->index], generation, __ATOMIC_RELEASE);
	return true;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	const struct cpu_topology* topology;
	struct cpu*                current;
	struct irq_state           state;
	uint64_t                   generation;
	uint64_t                   targets = 0u;

	state    = spinlock_lock_irqsave(&cache_sync_lock);
	topology = cpu_topology_get();
	current  = cpu_current();
	if (topology == NULL || topology->cpus == NULL || current == NULL || topology->cpu_count == 0u ||
	    topology->cpu_count > LOONGARCH_CACHE_MAX_CPUS || current->index >= LOONGARCH_CACHE_MAX_CPUS)
		hcf();

	hal_cache_sync_executable_range(address, size);
	generation = __atomic_load_n(&cache_request.generation, __ATOMIC_RELAXED) + 1u;
	if (generation == 0u) generation = 1u;
	cache_request.source_index = current->index;
	__atomic_store_n(&cache_request.generation, generation, __ATOMIC_RELEASE);
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];

		if (target == current || cpu_state_get(target) != CPU_STATE_ONLINE) continue;
		if (target->index >= LOONGARCH_CACHE_MAX_CPUS || !loongarch64_ipi_send(target, LOONGARCH64_IPI_VECTOR_CACHE))
			hcf();
		targets |= 1ull << target->index;
	}
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		if ((targets & (1ull << i)) == 0u) continue;
		while (__atomic_load_n(&cache_ack[i], __ATOMIC_ACQUIRE) != generation) spinlock_relax();
	}
	spinlock_unlock_irqrestore(&cache_sync_lock, state);
}
