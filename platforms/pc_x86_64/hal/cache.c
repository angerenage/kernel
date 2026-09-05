#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stdint.h>

#include "interrupts_private.h"

#define X86_CACHE_MAX_CPUS 64u

struct x86_cache_sync_request {
	size_t   source_index;
	uint64_t generation;
};

static struct x86_cache_sync_request x86_cache_request;
static uint64_t                      x86_cache_ack[X86_CACHE_MAX_CPUS];
static struct spinlock               x86_cache_sync_lock =
	SPINLOCK_INIT_CLASS("cache_sync_lock", SPINLOCK_ORDER_NONE, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static void x86_cache_serialize_local(void) {
	uint32_t eax = 0u;
	uint32_t ebx;
	uint32_t ecx = 0u;
	uint32_t edx;

	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx) : : "memory");
}

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	x86_cache_serialize_local();
}

bool x86_64_cache_handle_sync_nmi(void) {
	struct cpu* cpu = cpu_current();
	uint64_t    generation;

	if (cpu == NULL || cpu->index >= X86_CACHE_MAX_CPUS) hcf();
	generation = __atomic_load_n(&x86_cache_request.generation, __ATOMIC_ACQUIRE);
	if (generation == 0u || cpu->index == x86_cache_request.source_index ||
	    __atomic_load_n(&x86_cache_ack[cpu->index], __ATOMIC_ACQUIRE) == generation)
		return false;
	x86_cache_serialize_local();
	__atomic_store_n(&x86_cache_ack[cpu->index], generation, __ATOMIC_RELEASE);
	return true;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	const struct cpu_topology* topology;
	struct cpu*                current;
	struct irq_state           state;
	uint64_t                   targets = 0u;
	uint64_t                   generation;

	(void)address;
	(void)size;
	state    = spinlock_lock_irqsave(&x86_cache_sync_lock);
	current  = cpu_current();
	topology = cpu_topology_get();
	if (current == NULL || topology == NULL || topology->cpus == NULL || topology->cpu_count == 0u ||
	    topology->cpu_count > X86_CACHE_MAX_CPUS || current->index >= X86_CACHE_MAX_CPUS)
		hcf();

	x86_cache_serialize_local();
	generation = __atomic_load_n(&x86_cache_request.generation, __ATOMIC_RELAXED) + 1u;
	if (generation == 0u) generation = 1u;
	x86_cache_request.source_index = current->index;
	__atomic_store_n(&x86_cache_request.generation, generation, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];
		if (target == current || cpu_state_get(target) != CPU_STATE_ONLINE) continue;
		if (target->index >= X86_CACHE_MAX_CPUS || !apic_ipi_ready() || !apic_send_nmi((uint32_t)target->arch_id))
			hcf();
		targets |= 1ull << target->index;
	}
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		if ((targets & (1ull << i)) == 0u) continue;
		while (__atomic_load_n(&x86_cache_ack[i], __ATOMIC_ACQUIRE) != generation)
			__asm__ volatile("pause" : : : "memory");
	}
	spinlock_unlock_irqrestore(&x86_cache_sync_lock, state);
}
