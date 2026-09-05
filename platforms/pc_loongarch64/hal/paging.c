#include <base/math.h>
#include <core/cpu.h>
#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../paging_transaction.h"

#define LOONGARCH_CSR_CRMD 0x0u
#define LOONGARCH_CSR_PGDL 0x19u
#define LOONGARCH_CSR_PGDH 0x1au
#define LOONGARCH_CSR_PWCL 0x1cu
#define LOONGARCH_CSR_PWCH 0x1du
#define LOONGARCH_CSR_STLBPS 0x1eu

#define LOONGARCH_CRMD_DA (1u << 3)
#define LOONGARCH_CRMD_PG (1u << 4)
#define LOONGARCH_CRMD_DATF_SHIFT 5u
#define LOONGARCH_CRMD_DATM_SHIFT 7u
#define LOONGARCH_CRMD_CC 1u

#define LOONGARCH_PTE_V (1ull << 0)
#define LOONGARCH_PTE_D (1ull << 1)
#define LOONGARCH_PTE_PLV0 (0ull << 2)
#define LOONGARCH_PTE_PLV3 (3ull << 2)
#define LOONGARCH_PTE_PLV_MASK (3ull << 2)
#define LOONGARCH_PTE_MAT_SHIFT 4u
#define LOONGARCH_PTE_G (1ull << 6)
#define LOONGARCH_PTE_H (1ull << 6)
#define LOONGARCH_PTE_HG (1ull << 12)
#define LOONGARCH_PTE_P (1ull << 7)
#define LOONGARCH_PTE_W (1ull << 8)
#define LOONGARCH_PTE_NR (1ull << 61)
#define LOONGARCH_PTE_NX (1ull << 62)
#define LOONGARCH_PTE_RPLV (1ull << 63)

struct hal_paging_space {
	uintptr_t lower_root_phys;
	uintptr_t upper_root_phys;
	uint64_t  flags;
	uintptr_t storage_phys;
};

static struct hal_paging_info paging_info;

static bool                    initialized;
static uint64_t                phys_mask;
static unsigned                palen_bits;
static unsigned                valen_bits;
static unsigned                minimum_leaf_shift;
static struct hal_paging_space kernel_space;
static struct spinlock         paging_lock =
	SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static inline uint64_t loongarch_csrrd(unsigned csr) {
	uint64_t value;

	switch (csr) {
	case LOONGARCH_CSR_CRMD:
		__asm__ volatile("csrrd %0, 0x0" : "=r"(value));
		break;
	case LOONGARCH_CSR_PGDL:
		__asm__ volatile("csrrd %0, 0x19" : "=r"(value));
		break;
	case LOONGARCH_CSR_PGDH:
		__asm__ volatile("csrrd %0, 0x1a" : "=r"(value));
		break;
	case LOONGARCH_CSR_PWCL:
		__asm__ volatile("csrrd %0, 0x1c" : "=r"(value));
		break;
	case LOONGARCH_CSR_PWCH:
		__asm__ volatile("csrrd %0, 0x1d" : "=r"(value));
		break;
	case LOONGARCH_CSR_STLBPS:
		__asm__ volatile("csrrd %0, 0x1e" : "=r"(value));
		break;
	default:
		return 0;
	}

	return value;
}

static inline void loongarch_csrwr(uint64_t value, unsigned csr) {
	switch (csr) {
	case LOONGARCH_CSR_CRMD:
		__asm__ volatile("csrwr %0, 0x0" : : "r"(value) : "memory");
		break;
	case LOONGARCH_CSR_PGDL:
		__asm__ volatile("csrwr %0, 0x19" : : "r"(value) : "memory");
		break;
	case LOONGARCH_CSR_PGDH:
		__asm__ volatile("csrwr %0, 0x1a" : : "r"(value) : "memory");
		break;
	case LOONGARCH_CSR_PWCL:
		__asm__ volatile("csrwr %0, 0x1c" : : "r"(value) : "memory");
		break;
	case LOONGARCH_CSR_PWCH:
		__asm__ volatile("csrwr %0, 0x1d" : : "r"(value) : "memory");
		break;
	case LOONGARCH_CSR_STLBPS:
		__asm__ volatile("csrwr %0, 0x1e" : : "r"(value) : "memory");
		break;
	default:
		break;
	}
}

static inline uint32_t loongarch_cpucfg_word(unsigned word) {
	uint64_t value;
	__asm__ volatile("cpucfg %0, %1" : "=r"(value) : "r"((uint64_t)word));
	return (uint32_t)value;
}

static inline uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + boot_info.direct_map_offset;
}

static inline uintptr_t loongarch_entry_to_phys(uint64_t entry) {
	return (uintptr_t)(entry & phys_mask);
}

static inline uint64_t loongarch_entry_from_phys(uintptr_t phys) {
	return ((uint64_t)phys & phys_mask);
}

static inline size_t loongarch_leaf_size(unsigned level) {
	return (size_t)1u << (minimum_leaf_shift + 9u * level);
}

static inline void loongarch_tlb_flush_all(void) {
	__asm__ volatile("invtlb 0x0, $r0, $r0" ::: "memory");
}

static inline void loongarch_page_table_sync(void) {
	__asm__ volatile("dbar 0\n\t"
	                 "ibar 0" ::
	                     : "memory");
}

static inline void loongarch_tlb_shootdown_local(void) {
	const struct cpu_topology* topology = cpu_topology_get();
	struct cpu*                current  = cpu_current();

	/* pc_loongarch64 does not currently expose SMP (hal_cpu_prepare_smp()
	 * returns false), so every reachable paging update is single-CPU today. */
	if (topology == NULL || topology->cpus == NULL || current == NULL) hcf();
	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu* target = &topology->cpus[i];

		if (target != current && cpu_state_get(target) == CPU_STATE_ONLINE) hcf();
	}
	loongarch_page_table_sync();
	loongarch_tlb_flush_all();
}

static inline bool loongarch_upper_half(uintptr_t virt) {
	return ((uint64_t)virt >> (valen_bits - 1u)) != 0;
}

static inline bool loongarch_virtual_address_valid(uintptr_t virt) {
	uint64_t sign     = ((uint64_t)virt >> (valen_bits - 1u)) & 1u;
	uint64_t upper    = (uint64_t)virt >> valen_bits;
	uint64_t expected = sign != 0u ? ((1ull << (64u - valen_bits)) - 1u) : 0u;
	return upper == expected;
}

static inline uint64_t loongarch_root_phys(uintptr_t virt) {
	return loongarch_upper_half(virt) ? (loongarch_csrrd(LOONGARCH_CSR_PGDH) & phys_mask)
	                                  : (loongarch_csrrd(LOONGARCH_CSR_PGDL) & phys_mask);
}

static inline uint64_t loongarch_space_root_phys(const struct hal_paging_space* space, uintptr_t virt) {
	if (space == NULL) return loongarch_root_phys(virt);
	return loongarch_upper_half(virt) ? (space->upper_root_phys & phys_mask) : (space->lower_root_phys & phys_mask);
}

static inline uint64_t loongarch_leaf_flags(uint64_t flags, enum memory_type memory_type, bool huge) {
	uint64_t entry = LOONGARCH_PTE_P | LOONGARCH_PTE_V;
	uint64_t mat   = memory_type == MEMORY_TYPE_DEVICE ? 0ull : (uint64_t)LOONGARCH_CRMD_CC;

	entry |= ((flags & HAL_PAGE_USER) != 0) ? LOONGARCH_PTE_PLV3 : LOONGARCH_PTE_PLV0;
	entry |= mat << LOONGARCH_PTE_MAT_SHIFT;
	if ((flags & HAL_PAGE_WRITE) != 0) entry |= LOONGARCH_PTE_W | LOONGARCH_PTE_D;
	if ((flags & HAL_PAGE_READ) == 0) entry |= LOONGARCH_PTE_NR;
	if ((flags & HAL_PAGE_EXEC) == 0) entry |= LOONGARCH_PTE_NX;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= huge ? LOONGARCH_PTE_HG : LOONGARCH_PTE_G;
	if (huge) entry |= LOONGARCH_PTE_H;

	return entry;
}

static inline enum memory_type loongarch_leaf_memory_type(uint64_t entry) {
	return ((entry >> LOONGARCH_PTE_MAT_SHIFT) & 0x3u) == LOONGARCH_CRMD_CC ? MEMORY_TYPE_NORMAL : MEMORY_TYPE_DEVICE;
}

static inline uint64_t loongarch_leaf_apply_protection(uint64_t entry, uint64_t flags, bool huge) {
	entry &= ~(LOONGARCH_PTE_PLV_MASK | LOONGARCH_PTE_W | LOONGARCH_PTE_NR | LOONGARCH_PTE_NX |
	           (huge ? LOONGARCH_PTE_HG : LOONGARCH_PTE_G));
	entry |= (flags & HAL_PAGE_USER) != 0u ? LOONGARCH_PTE_PLV3 : LOONGARCH_PTE_PLV0;
	if ((flags & HAL_PAGE_WRITE) != 0u) entry |= LOONGARCH_PTE_W | LOONGARCH_PTE_D;
	if ((flags & HAL_PAGE_READ) == 0u) entry |= LOONGARCH_PTE_NR;
	if ((flags & HAL_PAGE_EXEC) == 0u) entry |= LOONGARCH_PTE_NX;
	if ((flags & HAL_PAGE_GLOBAL) != 0u) entry |= huge ? LOONGARCH_PTE_HG : LOONGARCH_PTE_G;
	return entry;
}

static void loongarch_transaction_restore(uint64_t* slot, uint64_t previous, void* context) {
	(void)context;
	*slot = previous;
}

static bool loongarch_walk_to_level(const struct hal_paging_space* space, uintptr_t virt, unsigned target_level,
                                    struct paging_transaction* transaction, uint64_t** out_slot) {
	uint64_t  root_phys = loongarch_space_root_phys(space, virt);
	uint64_t* table;

	if (!out_slot || target_level > 3u) return false;
	if (root_phys == 0) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)root_phys);

	for (unsigned level = 3u; level > target_level; level--) {
		size_t   index = (size_t)((virt >> (minimum_leaf_shift + 9u * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if (entry == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			if (!paging_transaction_record(transaction, &table[index], next_phys)) {
				(void)pmm_free_pages(next_phys, 1u);
				return false;
			}
			table[index] = loongarch_entry_from_phys(next_phys);
			entry        = table[index];
		}
		else if ((entry & LOONGARCH_PTE_H) != 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt(loongarch_entry_to_phys(entry));
	}

	*out_slot = &table[(virt >> (minimum_leaf_shift + 9u * target_level)) & 0x1ffu];
	return true;
}

bool hal_paging_init(void) {
	uint32_t         cpucfg1 = loongarch_cpucfg_word(1u);
	uint64_t         crmd;
	uint64_t         pwcl;
	uint64_t         pwch;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);

	palen_bits = ((cpucfg1 >> 4) & 0xffu) + 1u;
	valen_bits = ((cpucfg1 >> 12) & 0xffu) + 1u;
	if (((cpucfg1 >> 2) & 1u) == 0 || palen_bits < 40 || palen_bits > 60 || valen_bits < 40 || valen_bits > 48) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	minimum_leaf_shift            = 12u;
	paging_info.minimum_leaf_size = (size_t)1u << minimum_leaf_shift;
	paging_info.leaf_size_mask    = (1ull << minimum_leaf_shift) | (1ull << (minimum_leaf_shift + 9u));
	phys_mask                     = (((1ull << palen_bits) - 1u) & ~((1ull << minimum_leaf_shift) - 1u));
	if ((loongarch_csrrd(LOONGARCH_CSR_PGDH) & phys_mask) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	pwcl = (12ull << 0) | (9ull << 5) | (21ull << 10) | (9ull << 15) | (30ull << 20) | (9ull << 25);
	pwch = (39ull << 0) | (9ull << 6);
	loongarch_csrwr(pwcl, LOONGARCH_CSR_PWCL);
	loongarch_csrwr(pwch, LOONGARCH_CSR_PWCH);
	loongarch_csrwr(12ull, LOONGARCH_CSR_STLBPS);

	crmd = loongarch_csrrd(LOONGARCH_CSR_CRMD);
	crmd &= ~LOONGARCH_CRMD_DA;
	crmd |= LOONGARCH_CRMD_PG;
	crmd &= ~((uint64_t)3u << LOONGARCH_CRMD_DATF_SHIFT);
	crmd &= ~((uint64_t)3u << LOONGARCH_CRMD_DATM_SHIFT);
	crmd |= (uint64_t)LOONGARCH_CRMD_CC << LOONGARCH_CRMD_DATF_SHIFT;
	crmd |= (uint64_t)LOONGARCH_CRMD_CC << LOONGARCH_CRMD_DATM_SHIFT;
	loongarch_csrwr(crmd, LOONGARCH_CSR_CRMD);

	kernel_space = (struct hal_paging_space){
		.lower_root_phys = (uintptr_t)(loongarch_csrrd(LOONGARCH_CSR_PGDL) & phys_mask),
		.upper_root_phys = (uintptr_t)(loongarch_csrrd(LOONGARCH_CSR_PGDH) & phys_mask),
		.flags           = 0u,
	};
	initialized = true;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

static bool loongarch_protection_supported(uint64_t flags) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return loongarch_protection_supported(flags) && memory_type < MEMORY_TYPE_COUNT;
}

static bool loongarch_query_entry(const struct hal_paging_space* space, uintptr_t virt, uint64_t* out_entry,
                                  unsigned* out_shift) {
	uint64_t root_phys = loongarch_space_root_phys(space, virt);
	if (root_phys == 0u || out_entry == NULL || out_shift == NULL || !loongarch_virtual_address_valid(virt))
		return false;
	uint64_t* table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)root_phys);
	for (unsigned level = 3u;; level--) {
		unsigned shift = minimum_leaf_shift + 9u * level;
		uint64_t entry = table[(virt >> shift) & 0x1ffu];
		if (entry == 0u) return false;
		if (level == 0u || (entry & LOONGARCH_PTE_H) != 0u) {
			if ((entry & (LOONGARCH_PTE_P | LOONGARCH_PTE_V)) != (LOONGARCH_PTE_P | LOONGARCH_PTE_V)) return false;
			*out_entry = entry;
			*out_shift = shift;
			return true;
		}
		table = (uint64_t*)hhdm_phys_to_virt(loongarch_entry_to_phys(entry));
	}
	return false;
}

struct hal_paging_space* hal_paging_kernel_space(void) {
	return initialized ? &kernel_space : NULL;
}

bool hal_paging_space_create(struct hal_paging_space** out_space) {
	uintptr_t                root_phys    = 0;
	uintptr_t                storage_phys = 0;
	struct hal_paging_space* space;
	uint64_t*                root;
	struct irq_state         state;

	if (out_space == NULL || !initialized) return false;
	*out_space = NULL;

	state = spinlock_lock_irqsave(&paging_lock);
	if (!pmm_alloc_pages(1, &storage_phys) || !pmm_alloc_pages(1, &root_phys)) {
		if (storage_phys != 0u) (void)pmm_free_pages(storage_phys, 1u);
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	space = (struct hal_paging_space*)hhdm_phys_to_virt(storage_phys);

	root = (uint64_t*)hhdm_phys_to_virt(root_phys);
	memset(root, 0, PMM_PAGE_SIZE);
	*space = (struct hal_paging_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = kernel_space.upper_root_phys,
		.flags           = 0u,
		.storage_phys    = storage_phys,
	};
	*out_space = space;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static void loongarch_free_page_table_children(uint64_t* table, unsigned level) {
	if (table == NULL || level == 0u) return;
	for (size_t index = 0u; index < 512u; index++) {
		uint64_t entry = table[index];
		if (entry == 0u || (entry & LOONGARCH_PTE_H) != 0u) continue;

		uintptr_t child_phys = loongarch_entry_to_phys(entry);
		uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
		loongarch_free_page_table_children(child, level - 1u);
		(void)pmm_free_pages(child_phys, 1u);
	}
}

void hal_paging_space_destroy(struct hal_paging_space* space) {
	struct irq_state state;
	uintptr_t        root_phys;
	uint64_t*        root;

	if (space == NULL || space->lower_root_phys == 0u) return;
	root_phys = space->lower_root_phys & phys_mask;
	if (root_phys == kernel_space.lower_root_phys) return;
	state = spinlock_lock_irqsave(&paging_lock);
	root  = (uint64_t*)hhdm_phys_to_virt(root_phys);
	loongarch_free_page_table_children(root, 3u);
	(void)pmm_free_pages(root_phys, 1u);
	uintptr_t storage_phys = space->storage_phys;
	*space                 = (struct hal_paging_space){0};
	if (storage_phys != 0u) (void)pmm_free_pages(storage_phys, 1u);
	spinlock_unlock_irqrestore(&paging_lock, state);
}

bool hal_paging_activate(const struct hal_paging_space* space) {
	struct irq_state state;

	if (space == NULL || !initialized) return false;
	if (space->lower_root_phys == 0u && space->upper_root_phys == 0u) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	loongarch_csrwr(space->lower_root_phys & phys_mask, LOONGARCH_CSR_PGDL);
	loongarch_csrwr(space->upper_root_phys & phys_mask, LOONGARCH_CSR_PGDH);
	loongarch_page_table_sync();
	loongarch_tlb_flush_all();
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool loongarch_split_leaf(uint64_t* slot, unsigned level, struct paging_transaction* transaction) {
	uint64_t  entry = *slot;
	uintptr_t child_phys;
	uint64_t* child;
	size_t    parent_size = loongarch_leaf_size(level);
	size_t    child_size  = loongarch_leaf_size(level - 1u);
	uintptr_t base        = loongarch_entry_to_phys(entry) & ~(parent_size - 1u);
	uint64_t  flags       = entry & ~phys_mask & ~(LOONGARCH_PTE_H | LOONGARCH_PTE_HG);
	if (level != 1u || (entry & LOONGARCH_PTE_H) == 0u || !pmm_alloc_pages(1u, &child_phys)) return false;
	if ((entry & LOONGARCH_PTE_HG) != 0u) flags |= LOONGARCH_PTE_G;
	child = (uint64_t*)hhdm_phys_to_virt(child_phys);
	memset(child, 0, PMM_PAGE_SIZE);
	for (size_t index = 0u; index < 512u; index++)
		child[index] = loongarch_entry_from_phys(base + index * child_size) | flags;
	if (!paging_transaction_record(transaction, slot, child_phys)) {
		(void)pmm_free_pages(child_phys, 1u);
		return false;
	}
	*slot = loongarch_entry_from_phys(child_phys);
	return true;
}

static bool loongarch_table_empty(const uint64_t* table) {
	for (size_t index = 0u; index < 512u; index++) {
		if (table[index] != 0u) return false;
	}
	return true;
}

static bool loongarch_prepare_range(uint64_t* table, unsigned level, uintptr_t start, uintptr_t end,
                                    struct paging_transaction* transaction) {
	size_t span = loongarch_leaf_size(level);
	while (start < end) {
		size_t    index = (start >> (minimum_leaf_shift + 9u * level)) & 0x1ffu;
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t* slot  = &table[index];
		uint64_t  entry = *slot;
		if (entry != 0u && level > 0u) {
			if ((entry & LOONGARCH_PTE_H) != 0u) {
				uintptr_t leaf_start = start & ~(span - 1u);
				if (start != leaf_start || next != leaf_start + span) {
					if (!loongarch_split_leaf(slot, level, transaction)) return false;
					entry = *slot;
				}
			}
			if ((entry & LOONGARCH_PTE_H) == 0u &&
			    !loongarch_prepare_range(
					(uint64_t*)hhdm_phys_to_virt(loongarch_entry_to_phys(entry)), level - 1u, start, next, transaction))
				return false;
		}
		start = next;
	}
	return true;
}

static bool loongarch_change_range(uint64_t* table, unsigned level, uintptr_t start, uintptr_t end, bool protect,
                                   uint64_t flags, struct paging_transaction* transaction) {
	size_t span = loongarch_leaf_size(level);
	while (start < end) {
		size_t    index = (size_t)((start >> (minimum_leaf_shift + 9u * level)) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if (entry != 0u) {
			bool leaf = level == 0u || (entry & LOONGARCH_PTE_H) != 0u;
			if (leaf) {
				if ((entry & LOONGARCH_PTE_P) == 0u) return false;
				if (!paging_transaction_record(transaction, &table[index], 0u)) return false;
				if (protect) table[index] = loongarch_leaf_apply_protection(entry, flags, level != 0u);
				else table[index] = 0u;
			}
			else {
				uintptr_t child_phys = loongarch_entry_to_phys(entry);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!loongarch_change_range(child, level - 1u, start, next, protect, flags, transaction)) return false;
				if (!protect && loongarch_table_empty(child)) {
					if (!paging_transaction_retire(transaction, &table[index], child_phys)) return false;
					table[index] = 0u;
				}
			}
		}
		start = next;
	}
	return true;
}

static bool loongarch_range_args(struct hal_paging_space* space, uintptr_t virt, size_t size, uintptr_t* out_end) {
	uint64_t end;
	if (space == NULL || !initialized || size == 0u || !loongarch_virtual_address_valid(virt) ||
	    (virt & (paging_info.minimum_leaf_size - 1u)) != 0u || (size & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    add_overflow_u64(virt, size, &end) || !loongarch_virtual_address_valid((uintptr_t)end - 1u) ||
	    loongarch_upper_half(virt) != loongarch_upper_half((uintptr_t)end - 1u))
		return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	uintptr_t end;
	if (!loongarch_range_args(space, virt, size, &end)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uintptr_t                 root_phys   = (uintptr_t)loongarch_space_root_phys(space, virt);
	uint64_t*                 root        = root_phys == 0u ? NULL : (uint64_t*)hhdm_phys_to_virt(root_phys);
	struct paging_transaction transaction = {0};
	bool                      ok = root != NULL && loongarch_prepare_range(root, 3u, virt, end, &transaction) &&
	          loongarch_change_range(root, 3u, virt, end, false, 0u, &transaction);
	loongarch_tlb_shootdown_local();
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, loongarch_transaction_restore, NULL);
		loongarch_tlb_shootdown_local();
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	uintptr_t end;
	if (!loongarch_protection_supported(flags) || !loongarch_range_args(space, virt, size, &end)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uintptr_t                 root_phys   = (uintptr_t)loongarch_space_root_phys(space, virt);
	uint64_t*                 root        = root_phys == 0u ? NULL : (uint64_t*)hhdm_phys_to_virt(root_phys);
	struct paging_transaction transaction = {0};
	bool                      ok = root != NULL && loongarch_prepare_range(root, 3u, virt, end, &transaction) &&
	          loongarch_change_range(root, 3u, virt, end, true, flags, &transaction);
	loongarch_tlb_shootdown_local();
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, loongarch_transaction_restore, NULL);
		loongarch_tlb_shootdown_local();
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_query(const struct hal_paging_space* space, uintptr_t virt,
                      struct hal_paging_translation* out_translation) {
	uint64_t         entry;
	uint64_t         flags = 0;
	unsigned         shift;
	struct irq_state state;

	if (out_translation) *out_translation = (struct hal_paging_translation){0};

	if (space == NULL) return false;
	if (!initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!loongarch_query_entry(space, virt, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	uint64_t  leaf_mask        = ((uint64_t)1u << shift) - 1u;
	uintptr_t physical_address = (loongarch_entry_to_phys(entry) & ~leaf_mask) | (virt & leaf_mask);
	if ((entry & LOONGARCH_PTE_NR) == 0) flags |= HAL_PAGE_READ;
	if ((entry & LOONGARCH_PTE_W) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & LOONGARCH_PTE_NX) == 0) flags |= HAL_PAGE_EXEC;
	if ((shift == minimum_leaf_shift && (entry & LOONGARCH_PTE_G) != 0u) ||
	    (shift != minimum_leaf_shift && (entry & LOONGARCH_PTE_HG) != 0u))
		flags |= HAL_PAGE_GLOBAL;
	if ((entry & LOONGARCH_PTE_PLV_MASK) == LOONGARCH_PTE_PLV3) flags |= HAL_PAGE_USER;
	if (out_translation)
		*out_translation = (struct hal_paging_translation){
			physical_address, (size_t)1u << shift, flags, loongarch_leaf_memory_type(entry)};

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	uint64_t end;
	if (space == NULL || request == NULL || !initialized ||
	    !hal_paging_mapping_supported(request->flags, request->memory_type) || request->size == 0u ||
	    (request->virtual_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->physical_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->size & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    !loongarch_virtual_address_valid(request->virtual_address) ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address ||
	    ((request->physical_address + request->size - 1u) &
	     ~(phys_mask | ((uint64_t)paging_info.minimum_leaf_size - 1u))) != 0u ||
	    add_overflow_u64(request->virtual_address, request->size, &end) ||
	    !loongarch_virtual_address_valid((uintptr_t)end - 1u) ||
	    loongarch_upper_half(request->virtual_address) != loongarch_upper_half((uintptr_t)end - 1u))
		return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	struct paging_transaction transaction = {0};
	size_t                    mapped      = 0u;
	bool                      ok          = true;
	while (mapped < request->size) {
		uintptr_t virt      = request->virtual_address + mapped;
		uintptr_t phys      = request->physical_address + mapped;
		size_t    remaining = request->size - mapped;
		unsigned  level     = 1u;
		size_t    huge_size = loongarch_leaf_size(level);
		if ((virt & (huge_size - 1u)) != 0u || (phys & (huge_size - 1u)) != 0u || remaining < huge_size) level = 0u;
		uint64_t* slot;
		if (!loongarch_walk_to_level(space, virt, level, &transaction, &slot) || *slot != 0u ||
		    !paging_transaction_record(&transaction, slot, 0u)) {
			ok = false;
			break;
		}
		*slot = loongarch_entry_from_phys(phys & ~(loongarch_leaf_size(level) - 1u)) |
		        loongarch_leaf_flags(request->flags, request->memory_type, level != 0u);
		mapped += loongarch_leaf_size(level);
	}
	loongarch_tlb_shootdown_local();
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, loongarch_transaction_restore, NULL);
		loongarch_tlb_shootdown_local();
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}
