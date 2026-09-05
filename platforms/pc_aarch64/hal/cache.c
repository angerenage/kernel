#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <stdint.h>

#include "interrupts_private.h"

#define AARCH64_CACHE_MAX_CPUS 64u

struct aarch64_cache_sync_request {
	size_t   source_index;
	uint64_t generation;
};

static struct aarch64_cache_sync_request aarch64_cache_request;
static uint64_t                          aarch64_cache_ack[AARCH64_CACHE_MAX_CPUS];
static struct spinlock                   aarch64_cache_sync_lock =
	SPINLOCK_INIT_CLASS("cache_sync_lock", SPINLOCK_ORDER_NONE, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

void hal_cache_sync_executable_range(void* address, size_t size) {
	uint64_t  ctr;
	uintptr_t start = (uintptr_t)address;
	uintptr_t end   = start + size;
	size_t    dcache_line;
	size_t    icache_line;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	dcache_line = (size_t)4u << ((ctr >> 16u) & 0xfu);
	icache_line = (size_t)4u << (ctr & 0xfu);

	for (uintptr_t addr = start & ~(uintptr_t)(dcache_line - 1u); addr < end; addr += dcache_line) {
		__asm__ volatile("dc cvau, %0" : : "r"(addr) : "memory");
	}
	__asm__ volatile("dsb ish" : : : "memory");
	for (uintptr_t addr = start & ~(uintptr_t)(icache_line - 1u); addr < end; addr += icache_line) {
		__asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");
	}
	__asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

void aarch64_cache_poll_sync(void) {
	struct cpu* cpu = cpu_current();
	uint64_t    generation;

	if (cpu == NULL || cpu->index >= AARCH64_CACHE_MAX_CPUS) return;
	generation = __atomic_load_n(&aarch64_cache_request.generation, __ATOMIC_ACQUIRE);
	if (generation == 0u || cpu->index == aarch64_cache_request.source_index ||
	    __atomic_load_n(&aarch64_cache_ack[cpu->index], __ATOMIC_ACQUIRE) == generation)
		return;
	__asm__ volatile("isb" : : : "memory");
	__atomic_store_n(&aarch64_cache_ack[cpu->index], generation, __ATOMIC_RELEASE);
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	const struct cpu_topology* topology;
	struct cpu*                current;
	struct irq_state           state;
	uint64_t                   targets = 0u;
	uint64_t                   generation;

	state    = spinlock_lock_irqsave(&aarch64_cache_sync_lock);
	current  = cpu_current();
	topology = cpu_topology_get();
	if (current == NULL || topology == NULL || topology->cpus == NULL || topology->cpu_count == 0u ||
	    topology->cpu_count > AARCH64_CACHE_MAX_CPUS || current->index >= AARCH64_CACHE_MAX_CPUS)
		hcf();

	/* DC CVAU and IC IVAU make the cache effects visible to the Inner
	 * Shareable domain. Each other CPU still needs its own ISB. */
	hal_cache_sync_executable_range(address, size);
	generation = __atomic_load_n(&aarch64_cache_request.generation, __ATOMIC_RELAXED) + 1u;
	if (generation == 0u) generation = 1u;
	aarch64_cache_request.source_index = current->index;
	__atomic_store_n(&aarch64_cache_request.generation, generation, __ATOMIC_RELEASE);

	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];
		if (target == current || cpu_state_get(target) != CPU_STATE_ONLINE) continue;
		if (target->index >= AARCH64_CACHE_MAX_CPUS) hcf();
		targets |= 1ull << target->index;
	}
	/* Wake parked CPUs. Running CPUs acknowledge from their next exception. */
	__asm__ volatile("dsb ishst\n\tsev" : : : "memory");
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		if ((targets & (1ull << i)) == 0u) continue;
		while (__atomic_load_n(&aarch64_cache_ack[i], __ATOMIC_ACQUIRE) != generation)
			__asm__ volatile("yield" : : : "memory");
	}
	spinlock_unlock_irqrestore(&aarch64_cache_sync_lock, state);
}
