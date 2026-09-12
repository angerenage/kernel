#include <core/cpu.h>
#include <core/spinlock.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RISCV_SBI_EID_RFENCE 0x52464e43ul
#define RISCV_SBI_FID_REMOTE_FENCE_I 0ul

#define RISCV_FDT_MAGIC 0xd00dfeedu
#define RISCV_FDT_BEGIN_NODE 1u
#define RISCV_FDT_END_NODE 2u
#define RISCV_FDT_PROPERTY 3u
#define RISCV_FDT_NOP 4u
#define RISCV_FDT_END 9u
#define RISCV_FDT_MAX_DEPTH 32u

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

struct riscv_cache_fdt_node {
	const uint8_t* device_type;
	size_t         device_type_size;
	const uint8_t* status;
	size_t         status_size;
	const uint8_t* isa_extensions;
	size_t         isa_extensions_size;
	const uint8_t* isa;
	size_t         isa_size;
	const uint8_t* cbom_block_size;
	size_t         cbom_block_size_size;
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

static uint32_t riscv_fdt_u32(const void* data) {
	const uint8_t* bytes = data;
	return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static bool riscv_fdt_string_list_contains(const uint8_t* data, size_t size, const char* wanted) {
	if (data == NULL || wanted == NULL) return false;
	while (size != 0u) {
		size_t length = 0u;
		while (length < size && data[length] != '\0') length++;
		if (length == size) return false;
		if (strcmp((const char*)data, wanted) == 0) return true;
		data += length + 1u;
		size -= length + 1u;
	}
	return false;
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

static bool riscv_fdt_node_enabled(const struct riscv_cache_fdt_node* node) {
	return node->status == NULL || (node->status_size == 3u && memcmp(node->status, "ok\0", 3u) == 0) ||
	       (node->status_size == 5u && memcmp(node->status, "okay\0", 5u) == 0);
}

static bool riscv_fdt_node_is_cpu(const struct riscv_cache_fdt_node* node) {
	return node->device_type != NULL && node->device_type_size == 4u && memcmp(node->device_type, "cpu\0", 4u) == 0;
}

static bool riscv_zicbom_discover(size_t* out_block_size) {
	struct kernel_boot_data     dtb;
	struct riscv_cache_fdt_node stack[RISCV_FDT_MAX_DEPTH] = {0};
	if (out_block_size == NULL || !kernel_boot_dtb_get(&dtb) || dtb.address == NULL || dtb.size < 40u ||
	    riscv_fdt_u32(dtb.address) != RISCV_FDT_MAGIC)
		return false;

	const uint8_t* blob             = dtb.address;
	uint32_t       structure_offset = riscv_fdt_u32(blob + 8u);
	uint32_t       strings_offset   = riscv_fdt_u32(blob + 12u);
	uint32_t       strings_size     = riscv_fdt_u32(blob + 32u);
	uint32_t       structure_size   = riscv_fdt_u32(blob + 36u);
	if (structure_offset > dtb.size || structure_size > dtb.size - structure_offset || strings_offset > dtb.size ||
	    strings_size > dtb.size - strings_offset)
		return false;

	const uint8_t* cursor  = blob + structure_offset;
	const uint8_t* end     = cursor + structure_size;
	const uint8_t* strings = blob + strings_offset;
	size_t         depth   = 0u;
	size_t         cpus    = 0u;
	size_t         block   = 0u;

	while ((size_t)(end - cursor) >= 4u) {
		uint32_t token = riscv_fdt_u32(cursor);
		cursor += 4u;
		if (token == RISCV_FDT_BEGIN_NODE) {
			if (depth == RISCV_FDT_MAX_DEPTH) return false;
			const uint8_t* name_end = memchr(cursor, '\0', (size_t)(end - cursor));
			if (name_end == NULL) return false;
			stack[depth++]   = (struct riscv_cache_fdt_node){0};
			size_t name_size = (size_t)(name_end - cursor) + 1u;
			cursor += (name_size + 3u) & ~(size_t)3u;
		}
		else if (token == RISCV_FDT_END_NODE) {
			if (depth == 0u) return false;
			struct riscv_cache_fdt_node* node = &stack[depth - 1u];
			if (riscv_fdt_node_enabled(node) && riscv_fdt_node_is_cpu(node)) {
				bool has_zicbom =
					riscv_fdt_string_list_contains(node->isa_extensions, node->isa_extensions_size, "zicbom");
				if (!has_zicbom) has_zicbom = riscv_isa_string_has_zicbom(node->isa, node->isa_size);
				if (!has_zicbom || node->cbom_block_size == NULL || node->cbom_block_size_size != 4u) return false;
				size_t cpu_block = riscv_fdt_u32(node->cbom_block_size);
				if (cpu_block == 0u || (cpu_block & (cpu_block - 1u)) != 0u || (block != 0u && block != cpu_block))
					return false;
				block = cpu_block;
				cpus++;
			}
			depth--;
		}
		else if (token == RISCV_FDT_PROPERTY) {
			if (depth == 0u || (size_t)(end - cursor) < 8u) return false;
			uint32_t length      = riscv_fdt_u32(cursor);
			uint32_t name_offset = riscv_fdt_u32(cursor + 4u);
			cursor += 8u;
			if (length > (size_t)(end - cursor) || name_offset >= strings_size) return false;
			const char* name = (const char*)strings + name_offset;
			if (memchr(name, '\0', strings_size - name_offset) == NULL) return false;
			struct riscv_cache_fdt_node* node = &stack[depth - 1u];
			if (strcmp(name, "device_type") == 0) {
				node->device_type      = cursor;
				node->device_type_size = length;
			}
			else if (strcmp(name, "status") == 0) {
				node->status      = cursor;
				node->status_size = length;
			}
			else if (strcmp(name, "riscv,isa-extensions") == 0) {
				node->isa_extensions      = cursor;
				node->isa_extensions_size = length;
			}
			else if (strcmp(name, "riscv,isa") == 0) {
				node->isa      = cursor;
				node->isa_size = length;
			}
			else if (strcmp(name, "riscv,cbom-block-size") == 0) {
				node->cbom_block_size      = cursor;
				node->cbom_block_size_size = length;
			}
			cursor += (length + 3u) & ~(size_t)3u;
		}
		else if (token == RISCV_FDT_NOP) continue;
		else if (token == RISCV_FDT_END) break;
		else return false;
		if (cursor > end) return false;
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
