#include <core/cpu.h>
#include <core/spinlock.h>
#include <firmware/dt/device.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RISCV_SBI_EID_RFENCE 0x52464e43ul
#define RISCV_SBI_FID_REMOTE_FENCE_I 0ul

enum riscv_zicbom_state {
	RISCV_ZICBOM_UNKNOWN = 0u,
	RISCV_ZICBOM_PROBING,
	RISCV_ZICBOM_UNAVAILABLE,
	RISCV_ZICBOM_AVAILABLE,
};

struct riscv_cache_sbi_ret {
	long error;
	long value;
};

static uint32_t riscv_zicbom_state;
static size_t   riscv_zicbom_block_size;

static struct riscv_cache_sbi_ret riscv_cache_sbi_call2(unsigned long arg0, unsigned long arg1, unsigned long fid,
                                                        unsigned long eid) {
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a7 asm("a7") = eid;

	__asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory", "a2", "a3", "a4", "a5");
	return (struct riscv_cache_sbi_ret){.error = (long)a0, .value = (long)a1};
}

static bool riscv_isa_string_has_zicbom(const uint8_t* data, size_t size) {
	static const char name[] = "zicbom";
	if (data == NULL || size == 0u || data[size - 1u] != '\0') return false;
	const char* isa = (const char*)data;
	const char* end = isa + size - 1u;
	for (const char* token = isa; token < end;) {
		const char* token_end = token;
		while (token_end < end && *token_end != '_') token_end++;
		size_t token_size = (size_t)(token_end - token);
		if (token_size >= sizeof(name) - 1u && memcmp(token, name, sizeof(name) - 1u) == 0 &&
		    (token_size == sizeof(name) - 1u || (token[sizeof(name) - 1u] >= '0' && token[sizeof(name) - 1u] <= '9')))
			return true;
		token = token_end < end ? token_end + 1u : end;
	}
	return false;
}

static bool riscv_zicbom_discover(size_t* out_block_size) {
	size_t cpus  = 0u;
	size_t block = 0u;

	if (out_block_size == NULL) return false;
	for (size_t index = 0u;; index++) {
		struct dt_node     node = dt_node_with_string_at("device_type", "cpu", index);
		struct dt_property extensions;
		struct dt_property isa;
		struct dt_property block_size;
		uint64_t           value;
		bool               has_zicbom;

		if (!dt_node_valid(node)) break;
		if (!dt_node_enabled(node)) continue;
		has_zicbom = dt_node_property(node, "riscv,isa-extensions", &extensions) &&
		             dt_property_string_list_contains(&extensions, "zicbom");
		if (!has_zicbom && dt_node_property(node, "riscv,isa", &isa))
			has_zicbom = riscv_isa_string_has_zicbom(isa.data, isa.size);
		if (!has_zicbom || !dt_node_property(node, "riscv,cbom-block-size", &block_size) || block_size.size != 4u ||
		    !dt_property_read_cells(&block_size, 0u, 1u, &value) || value == 0u || (value & (value - 1u)) != 0u ||
		    value > SIZE_MAX || (block != 0u && block != value))
			return false;
		block = (size_t)value;
		cpus++;
	}
	if (cpus == 0u || block == 0u) return false;
	*out_block_size = block;
	return true;
}

static bool riscv_zicbom_available(size_t* out_block_size) {
	uint32_t state;
	if (out_block_size == NULL) return false;
	for (;;) {
		state = __atomic_load_n(&riscv_zicbom_state, __ATOMIC_ACQUIRE);
		if (state == RISCV_ZICBOM_AVAILABLE) {
			*out_block_size = riscv_zicbom_block_size;
			return true;
		}
		if (state == RISCV_ZICBOM_UNAVAILABLE) return false;
		if (state == RISCV_ZICBOM_PROBING) {
			spinlock_relax();
			continue;
		}
		uint32_t expected = RISCV_ZICBOM_UNKNOWN;
		if (__atomic_compare_exchange_n(
				&riscv_zicbom_state, &expected, RISCV_ZICBOM_PROBING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
	}
	size_t block_size;
	if (!riscv_zicbom_discover(&block_size)) {
		__atomic_store_n(&riscv_zicbom_state, RISCV_ZICBOM_UNAVAILABLE, __ATOMIC_RELEASE);
		return false;
	}
	riscv_zicbom_block_size = block_size;
	__atomic_store_n(&riscv_zicbom_state, RISCV_ZICBOM_AVAILABLE, __ATOMIC_RELEASE);
	*out_block_size = block_size;
	return true;
}

static inline void riscv_cbo_flush(uintptr_t address) {
	__asm__ volatile(".insn i 0x0f, 0x2, x0, %0, 2" : : "r"(address) : "memory");
}

static inline void riscv_cbo_inval(uintptr_t address) {
	__asm__ volatile(".insn i 0x0f, 0x2, x0, %0, 0" : : "r"(address) : "memory");
}

static bool riscv_cache_sync_dma_range(void* address, size_t size, bool for_device) {
	uintptr_t start = (uintptr_t)address;
	uintptr_t first;
	uintptr_t end;
	uintptr_t limit;
	size_t    block;
	if (size == 0u) return true;
	if (address == NULL || size > UINTPTR_MAX - start || !riscv_zicbom_available(&block)) return false;
	first = start & ~((uintptr_t)block - 1u);
	end   = start + size;
	if (end > UINTPTR_MAX - (block - 1u)) return false;
	limit = (end + block - 1u) & ~((uintptr_t)block - 1u);

	__asm__ volatile("fence iorw, iorw" : : : "memory");
	for (uintptr_t current = first; current < limit; current += block) {
		if (for_device) riscv_cbo_flush(current);
		else riscv_cbo_inval(current);
	}
	__asm__ volatile("fence iorw, iorw" : : : "memory");
	return true;
}

bool hal_cache_sync_for_device(void* address, size_t size) {
	return riscv_cache_sync_dma_range(address, size, true);
}

bool hal_cache_sync_for_cpu(void* address, size_t size) {
	return riscv_cache_sync_dma_range(address, size, false);
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
