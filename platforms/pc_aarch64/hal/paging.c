#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AARCH64_DESC_VALID (1ull << 0)
#define AARCH64_DESC_TABLE (1ull << 1)
#define AARCH64_PAGE (1ull << 1)
#define AARCH64_ATTR_MASK (7ull << 2)
#define AARCH64_AP_MASK (3ull << 6)
#define AARCH64_AP_RW_EL1 (0ull << 6)
#define AARCH64_AP_RO_EL1 (2ull << 6)
#define AARCH64_SH_MASK (3ull << 8)
#define AARCH64_AF (1ull << 10)
#define AARCH64_NG (1ull << 11)
#define AARCH64_ADDR_MASK 0x0000fffffffff000ull
#define AARCH64_PXN (1ull << 53)
#define AARCH64_UXN (1ull << 54)
#define AARCH64_LOWER_MASK 0x0000000000000fffull
#define AARCH64_UPPER_MASK (AARCH64_PXN | AARCH64_UXN)
#define AARCH64_TCR_T0SZ_MASK 0x3full
#define AARCH64_TCR_T1SZ_SHIFT 16u
#define AARCH64_TCR_T1SZ_MASK (0x3full << AARCH64_TCR_T1SZ_SHIFT)
#define AARCH64_TCR_TG0_SHIFT 14u
#define AARCH64_TCR_TG0_MASK (3ull << AARCH64_TCR_TG0_SHIFT)
#define AARCH64_TCR_TG1_SHIFT 30u
#define AARCH64_TCR_TG1_MASK (3ull << AARCH64_TCR_TG1_SHIFT)
#define AARCH64_TCR_TG_4K 0ull
#define AARCH64_TCR_TG1_4K 2ull
#define AARCH64_DEVICE_ATTR (2ull << 2)

static bool            initialized;
static uint64_t        normal_attrs_template;
static struct spinlock paging_lock =
	SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

struct aarch64_walk_params {
	uint64_t root_phys;
	uint64_t va_bits;
	unsigned levels;
};

static inline uint64_t aarch64_read_ttbr0_el1(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(value));
	return value;
}

static inline uint64_t aarch64_read_ttbr1_el1(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, ttbr1_el1" : "=r"(value));
	return value;
}

static inline uint64_t aarch64_read_tcr_el1(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, tcr_el1" : "=r"(value));
	return value;
}

static inline uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + boot_info.direct_map_offset;
}

static inline void aarch64_tlb_flush_all(void) {
	__asm__ volatile("dsb ishst\n\t"
	                 "tlbi vmalle1is\n\t"
	                 "dsb ish\n\t"
	                 "isb" ::
	                     : "memory");
}

static bool aarch64_is_upper_region(uintptr_t virt) {
	return ((uint64_t)virt >> 63) != 0;
}

static unsigned aarch64_level_count(uint64_t va_bits) {
	if (va_bits <= 12) return 1;
	return (unsigned)(((va_bits - 12) + 8) / 9);
}

static bool aarch64_walk_params(uintptr_t virt, struct aarch64_walk_params* out) {
	uint64_t tcr;
	uint64_t root_phys;
	uint64_t va_bits;
	uint64_t granule;

	if (!out) return false;

	tcr = aarch64_read_tcr_el1();
	if (aarch64_is_upper_region(virt)) {
		granule = (tcr & AARCH64_TCR_TG1_MASK) >> AARCH64_TCR_TG1_SHIFT;
		if (granule != AARCH64_TCR_TG1_4K) return false;
		va_bits   = 64u - ((tcr & AARCH64_TCR_T1SZ_MASK) >> AARCH64_TCR_T1SZ_SHIFT);
		root_phys = aarch64_read_ttbr1_el1() & AARCH64_ADDR_MASK;
	}
	else {
		granule = (tcr & AARCH64_TCR_TG0_MASK) >> AARCH64_TCR_TG0_SHIFT;
		if (granule != AARCH64_TCR_TG_4K) return false;
		va_bits   = 64u - (tcr & AARCH64_TCR_T0SZ_MASK);
		root_phys = aarch64_read_ttbr0_el1() & AARCH64_ADDR_MASK;
	}

	if (root_phys == 0) return false;
	if (va_bits < 12 || va_bits > 48) return false;

	*out = (struct aarch64_walk_params){
		.root_phys = root_phys,
		.va_bits   = va_bits,
		.levels    = aarch64_level_count(va_bits),
	};
	return out->levels >= 1 && out->levels <= 4;
}

static bool aarch64_lookup_entry(uintptr_t virt, bool create, uint64_t** out_table, size_t* out_index,
                                 unsigned* out_shift) {
	struct aarch64_walk_params params;
	uint64_t*                  table;

	if (!out_table || !out_index || !out_shift) return false;
	if (!aarch64_walk_params(virt, &params)) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);

	for (unsigned level = params.levels - 1; level > 0; level--) {
		size_t   index = (size_t)((virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & AARCH64_DESC_VALID) == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!create) return false;
			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			table[index] = ((uint64_t)next_phys & AARCH64_ADDR_MASK) | AARCH64_DESC_VALID | AARCH64_DESC_TABLE;
			entry        = table[index];
		}
		else if ((entry & AARCH64_DESC_TABLE) == 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & AARCH64_ADDR_MASK));
	}

	*out_table = table;
	*out_index = (size_t)((virt >> 12) & 0x1ffu);
	*out_shift = 12;
	return true;
}

static bool aarch64_query_entry(uintptr_t virt, uint64_t* out_entry, unsigned* out_shift) {
	struct aarch64_walk_params params;
	uint64_t*                  table;

	if (!out_entry || !out_shift) return false;
	if (!aarch64_walk_params(virt, &params)) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);

	for (unsigned level = params.levels - 1;; level--) {
		size_t   index = (size_t)((virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & AARCH64_DESC_VALID) == 0) return false;

		if (level == 0 || (entry & AARCH64_DESC_TABLE) == 0) {
			*out_entry = entry;
			*out_shift = 12 + 9 * level;
			return true;
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & AARCH64_ADDR_MASK));
	}
}

static uint64_t aarch64_leaf_flags(uint64_t flags) {
	uint64_t entry = normal_attrs_template;

	entry &= ~(AARCH64_AP_MASK | AARCH64_NG | AARCH64_PXN | AARCH64_UXN | AARCH64_ATTR_MASK);
	entry |= AARCH64_AF | AARCH64_UXN;
	entry |= ((flags & HAL_PAGE_WRITE) != 0) ? AARCH64_AP_RW_EL1 : AARCH64_AP_RO_EL1;
	entry |= ((flags & HAL_PAGE_GLOBAL) != 0) ? 0ull : AARCH64_NG;
	entry |= ((flags & HAL_PAGE_EXEC) != 0) ? 0ull : AARCH64_PXN;

	if ((flags & HAL_PAGE_NO_CACHE) != 0) {
		entry &= ~(AARCH64_ATTR_MASK | AARCH64_SH_MASK);
		entry |= AARCH64_DEVICE_ATTR;
	}

	return entry | AARCH64_DESC_VALID | AARCH64_PAGE;
}

bool hal_paging_init(void) {
	uint64_t         entry;
	unsigned         shift;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);

	initialized           = false;
	normal_attrs_template = 0;

	if (!aarch64_query_entry((uintptr_t)&hal_paging_init, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	(void)shift;

	normal_attrs_template = entry & (AARCH64_LOWER_MASK | AARCH64_UPPER_MASK);
	initialized           = true;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	unsigned         shift = 0;
	struct irq_state state;

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!aarch64_lookup_entry(virt, true, &table, &index, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & AARCH64_DESC_VALID) != 0 || shift != 12) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = ((uint64_t)phys & AARCH64_ADDR_MASK) | aarch64_leaf_flags(flags);
	aarch64_tlb_flush_all();
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_unmap(uintptr_t virt) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	unsigned         shift = 0;
	struct irq_state state;

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!aarch64_lookup_entry(virt, false, &table, &index, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & AARCH64_DESC_VALID) == 0 || shift != 12) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = 0;
	aarch64_tlb_flush_all();
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	uint64_t         entry;
	uint64_t         flags = 0;
	unsigned         shift;
	uint64_t         page_mask;
	struct irq_state state;

	if (out_phys) *out_phys = 0;
	if (out_flags) *out_flags = 0;

	if (!initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!aarch64_query_entry(virt, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	page_mask = (1ull << shift) - 1u;
	if (out_phys) *out_phys = (uintptr_t)((entry & AARCH64_ADDR_MASK) | ((uint64_t)virt & page_mask));

	if ((entry & AARCH64_AP_MASK) == AARCH64_AP_RW_EL1) flags |= HAL_PAGE_WRITE;
	if ((entry & AARCH64_PXN) == 0) flags |= HAL_PAGE_EXEC;
	if ((entry & AARCH64_NG) == 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & AARCH64_ATTR_MASK) == AARCH64_DEVICE_ATTR) flags |= HAL_PAGE_NO_CACHE;
	if (out_flags) *out_flags = flags;

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}
