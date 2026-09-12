#include <core/cpu.h>
#include <hal/cache.h>
#include <hal/hcf.h>

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

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	const struct cpu_topology* topology = cpu_topology_get();
	struct cpu*                current  = cpu_current();

	if (topology == NULL || topology->cpus == NULL || current == NULL) hcf();
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];
		if (target != current && cpu_state_get(target) == CPU_STATE_ONLINE) hcf();
	}
	hal_cache_sync_executable_range(address, size);
}
