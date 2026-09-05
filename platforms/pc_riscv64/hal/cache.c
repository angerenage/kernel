#include <core/cpu.h>
#include <hal/cache.h>
#include <hal/hcf.h>

#define RISCV_SBI_EID_RFENCE 0x52464e43ul
#define RISCV_SBI_FID_REMOTE_FENCE_I 0ul

struct riscv_cache_sbi_ret {
	long error;
	long value;
};

static struct riscv_cache_sbi_ret riscv_cache_sbi_call2(unsigned long arg0, unsigned long arg1, unsigned long fid,
                                                        unsigned long eid) {
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a7 asm("a7") = eid;

	__asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory", "a2", "a3", "a4", "a5");
	return (struct riscv_cache_sbi_ret){.error = (long)a0, .value = (long)a1};
}

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	__asm__ volatile("fence.i" : : : "memory");
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	const struct cpu_topology* topology = cpu_topology_get();
	struct cpu*                current  = cpu_current();

	(void)address;
	(void)size;
	if (topology == NULL || topology->cpus == NULL || current == NULL || topology->cpu_count == 0u) hcf();
	/* The writing hart must publish its stores before remote harts execute FENCE.I. */
	__asm__ volatile("fence rw, rw" : : : "memory");
	hal_cache_sync_executable_range(address, size);
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];
		if (target == current || cpu_state_get(target) != CPU_STATE_ONLINE) continue;
		if (riscv_cache_sbi_call2(
				1ul, (unsigned long)target->arch_id, RISCV_SBI_FID_REMOTE_FENCE_I, RISCV_SBI_EID_RFENCE)
		        .error != 0)
			hcf();
	}
}
