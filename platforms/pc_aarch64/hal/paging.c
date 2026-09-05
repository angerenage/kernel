#include <base/math.h>
#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../paging_transaction.h"

#define AARCH64_DESC_VALID (1ull << 0)
#define AARCH64_DESC_TABLE (1ull << 1)
#define AARCH64_PAGE (1ull << 1)
#define AARCH64_ATTR_MASK (7ull << 2)
#define AARCH64_AP_MASK (3ull << 6)
#define AARCH64_AP_RW_EL1 (0ull << 6)
#define AARCH64_AP_RW_EL0 (1ull << 6)
#define AARCH64_AP_RO_EL1 (2ull << 6)
#define AARCH64_AP_RO_EL0 (3ull << 6)
#define AARCH64_SH_MASK (3ull << 8)
#define AARCH64_AF (1ull << 10)
#define AARCH64_NG (1ull << 11)
#define AARCH64_PHYS_FIELD_MASK 0x0000ffffffffffffull
#define AARCH64_DBM (1ull << 51)
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
#define AARCH64_TCR_IPS_SHIFT 32u
#define AARCH64_TCR_IPS_MASK (7ull << AARCH64_TCR_IPS_SHIFT)
#define AARCH64_TCR_DS (1ull << 59)
#define AARCH64_DEVICE_ATTR (2ull << 2)
#define AARCH64_MAIR_DEVICE_NGNRNE 0x00u

struct hal_paging_space {
	uintptr_t lower_root_phys;
	uintptr_t upper_root_phys;
	uint64_t  flags;
	uintptr_t storage_phys;
};

static struct hal_paging_info paging_info;

static bool                    initialized;
static bool                    device_memory_supported;
static unsigned                minimum_leaf_shift;
static uint64_t                pte_address_mask;
static uint64_t                normal_attrs_template;
static struct hal_paging_space kernel_space;
static struct spinlock         paging_lock =
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

static inline uint64_t aarch64_read_mair_el1(void) {
	uint64_t value;
	__asm__ volatile("mrs %0, mair_el1" : "=r"(value));
	return value;
}

static inline size_t aarch64_leaf_size(unsigned level) {
	return (size_t)1u << (minimum_leaf_shift + 9u * level);
}

static inline void aarch64_write_ttbr0_el1(uint64_t value) {
	__asm__ volatile("msr ttbr0_el1, %0\n\tisb" : : "r"(value) : "memory");
}

static inline void aarch64_write_ttbr1_el1(uint64_t value) {
	__asm__ volatile("msr ttbr1_el1, %0\n\tisb" : : "r"(value) : "memory");
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

static void aarch64_replace_descriptor(uint64_t* slot, uint64_t value) {
	uint64_t previous = *slot;
	if (previous == value) return;
	if ((previous & AARCH64_DESC_VALID) != 0u && (value & AARCH64_DESC_VALID) != 0u) {
		/* Break-before-make: make the old descriptor invalid and ensure every PE
		 * has discarded translations
		 * derived from it before installing the replacement. */
		*slot = 0u;
		aarch64_tlb_flush_all();
		*slot = value;
		__asm__ volatile("dsb ishst\n\t"
		                 "isb" ::
		                     : "memory");
	}
	else {
		*slot = value;
	}
}

static void aarch64_transaction_restore(uint64_t* slot, uint64_t previous, void* context) {
	(void)context;
	if ((*slot & AARCH64_DESC_VALID) == 0u && (previous & AARCH64_DESC_VALID) != 0u) {
		/* A failed operation may have performed the break already. Complete the
		 * invalid-descriptor
		 * synchronization before making the old descriptor valid again. */
		aarch64_tlb_flush_all();
		*slot = previous;
		__asm__ volatile("dsb ishst\n\t"
		                 "isb" ::
		                     : "memory");
		return;
	}
	aarch64_replace_descriptor(slot, previous);
}

static void aarch64_tlb_shootdown_range(uintptr_t start, size_t size) {
	/* VAAE1IS invalidates by VA for all ASIDs in the inner-shareable domain,
	 * so this is an architectural SMP shootdown rather than a CPU-local flush. */
	__asm__ volatile("dsb ishst" : : : "memory");
	for (size_t offset = 0u; offset < size; offset += paging_info.minimum_leaf_size) {
		uint64_t operand = (((uint64_t)start + offset) >> minimum_leaf_shift) & ((1ull << 44u) - 1u);

		__asm__ volatile("tlbi vaae1is, %0" : : "r"(operand) : "memory");
	}
	__asm__ volatile("dsb ish\n\t"
	                 "isb" ::
	                     : "memory");
}

static bool aarch64_is_upper_region(uintptr_t virt) {
	return ((uint64_t)virt >> 63) != 0;
}

static unsigned aarch64_level_count(uint64_t va_bits) {
	if (va_bits <= minimum_leaf_shift) return 1;
	return (unsigned)(((va_bits - minimum_leaf_shift) + 8u) / 9u);
}

static bool aarch64_walk_params_for_space(const struct hal_paging_space* space, uintptr_t virt,
                                          struct aarch64_walk_params* out) {
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
		root_phys = space != NULL ? space->upper_root_phys : (aarch64_read_ttbr1_el1() & pte_address_mask);
	}
	else {
		granule = (tcr & AARCH64_TCR_TG0_MASK) >> AARCH64_TCR_TG0_SHIFT;
		if (granule != AARCH64_TCR_TG_4K) return false;
		va_bits   = 64u - (tcr & AARCH64_TCR_T0SZ_MASK);
		root_phys = space != NULL ? space->lower_root_phys : (aarch64_read_ttbr0_el1() & pte_address_mask);
	}

	if (root_phys == 0) return false;
	if (va_bits < minimum_leaf_shift || va_bits > 48) return false;
	uint64_t upper          = (uint64_t)virt >> va_bits;
	uint64_t expected_upper = aarch64_is_upper_region(virt) ? ((1ull << (64u - va_bits)) - 1u) : 0u;
	if (upper != expected_upper) return false;

	*out = (struct aarch64_walk_params){
		.root_phys = root_phys,
		.va_bits   = va_bits,
		.levels    = aarch64_level_count(va_bits),
	};
	return out->levels >= 1 && out->levels <= 4;
}

static bool aarch64_walk_to_level(const struct hal_paging_space* space, uintptr_t virt, unsigned target_level,
                                  struct paging_transaction* transaction, uint64_t** out_slot) {
	struct aarch64_walk_params params;
	uint64_t*                  table;

	if (!out_slot) return false;
	if (!aarch64_walk_params_for_space(space, virt, &params)) return false;
	if (target_level >= params.levels) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);

	for (unsigned level = params.levels - 1; level > target_level; level--) {
		size_t   index = (size_t)((virt >> (minimum_leaf_shift + 9u * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & AARCH64_DESC_VALID) == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			if (!paging_transaction_record(transaction, &table[index], next_phys)) {
				(void)pmm_free_pages(next_phys, 1u);
				return false;
			}
			table[index] = ((uint64_t)next_phys & pte_address_mask) | AARCH64_DESC_VALID | AARCH64_DESC_TABLE;
			entry        = table[index];
		}
		else if ((entry & AARCH64_DESC_TABLE) == 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & pte_address_mask));
	}

	*out_slot = &table[(virt >> (minimum_leaf_shift + 9u * target_level)) & 0x1ffu];
	return true;
}

static bool aarch64_query_entry_in(const struct hal_paging_space* space, uintptr_t virt, uint64_t* out_entry,
                                   unsigned* out_shift) {
	struct aarch64_walk_params params;
	uint64_t*                  table;

	if (!out_entry || !out_shift) return false;
	if (!aarch64_walk_params_for_space(space, virt, &params)) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);

	for (unsigned level = params.levels - 1;; level--) {
		size_t   index = (size_t)((virt >> (minimum_leaf_shift + 9u * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & AARCH64_DESC_VALID) == 0) return false;

		if (level == 0 || (entry & AARCH64_DESC_TABLE) == 0) {
			*out_entry = entry;
			*out_shift = minimum_leaf_shift + 9u * level;
			return true;
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & pte_address_mask));
	}
}

static bool aarch64_query_entry(uintptr_t virt, uint64_t* out_entry, unsigned* out_shift) {
	return aarch64_query_entry_in(NULL, virt, out_entry, out_shift);
}

static uint64_t aarch64_leaf_flags(uint64_t flags, enum memory_type memory_type) {
	uint64_t entry = normal_attrs_template;

	entry &= ~(AARCH64_DESC_TABLE | AARCH64_AP_MASK | AARCH64_NG | AARCH64_PXN | AARCH64_UXN | AARCH64_ATTR_MASK);
	entry |= AARCH64_AF;
	if ((flags & HAL_PAGE_USER) != 0) {
		entry |= ((flags & HAL_PAGE_WRITE) != 0) ? AARCH64_AP_RW_EL0 : AARCH64_AP_RO_EL0;
		entry |= AARCH64_PXN;
		if ((flags & HAL_PAGE_EXEC) == 0) entry |= AARCH64_UXN;
	}
	else {
		entry |= ((flags & HAL_PAGE_WRITE) != 0) ? AARCH64_AP_RW_EL1 : AARCH64_AP_RO_EL1;
		entry |= AARCH64_UXN;
		if ((flags & HAL_PAGE_EXEC) == 0) entry |= AARCH64_PXN;
	}
	entry |= ((flags & HAL_PAGE_GLOBAL) != 0) ? 0ull : AARCH64_NG;

	if (memory_type == MEMORY_TYPE_DEVICE) {
		entry &= ~(AARCH64_ATTR_MASK | AARCH64_SH_MASK);
		entry |= AARCH64_DEVICE_ATTR;
	}

	return entry | AARCH64_DESC_VALID;
}

static enum memory_type aarch64_leaf_memory_type(uint64_t entry) {
	return (entry & AARCH64_ATTR_MASK) == AARCH64_DEVICE_ATTR ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL;
}

static uint64_t aarch64_leaf_apply_protection(uint64_t entry, uint64_t flags) {
	entry &= ~(AARCH64_AP_MASK | AARCH64_NG | AARCH64_PXN | AARCH64_UXN);
	if ((flags & HAL_PAGE_WRITE) == 0u) entry &= ~AARCH64_DBM;
	if ((flags & HAL_PAGE_USER) != 0u) {
		entry |= (flags & HAL_PAGE_WRITE) != 0u ? AARCH64_AP_RW_EL0 : AARCH64_AP_RO_EL0;
		entry |= AARCH64_PXN;
		if ((flags & HAL_PAGE_EXEC) == 0u) entry |= AARCH64_UXN;
	}
	else {
		entry |= (flags & HAL_PAGE_WRITE) != 0u ? AARCH64_AP_RW_EL1 : AARCH64_AP_RO_EL1;
		entry |= AARCH64_UXN;
		if ((flags & HAL_PAGE_EXEC) == 0u) entry |= AARCH64_PXN;
	}
	if ((flags & HAL_PAGE_GLOBAL) == 0u) entry |= AARCH64_NG;
	return entry;
}

static bool aarch64_physical_bits(uint64_t tcr, unsigned* out_bits) {
	if (out_bits == NULL || (tcr & AARCH64_TCR_DS) != 0u) return false;
	switch ((tcr & AARCH64_TCR_IPS_MASK) >> AARCH64_TCR_IPS_SHIFT) {
	case 0u:
		*out_bits = 32u;
		return true;
	case 1u:
		*out_bits = 36u;
		return true;
	case 2u:
		*out_bits = 40u;
		return true;
	case 3u:
		*out_bits = 42u;
		return true;
	case 4u:
		*out_bits = 44u;
		return true;
	case 5u:
		*out_bits = 48u;
		return true;
	default:
		/* The 52-bit formats require LPA/LPA2 descriptor handling. */
		return false;
	}
}

bool hal_paging_init(void) {
	uint64_t         entry;
	uint64_t         tcr;
	unsigned         shift;
	unsigned         physical_bits;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);

	initialized                   = false;
	normal_attrs_template         = 0;
	minimum_leaf_shift            = 12u;
	paging_info.minimum_leaf_size = (size_t)1u << minimum_leaf_shift;
	tcr                           = aarch64_read_tcr_el1();
	if (!aarch64_physical_bits(tcr, &physical_bits)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	pte_address_mask =
		AARCH64_PHYS_FIELD_MASK & ((1ull << physical_bits) - 1u) & ~((uint64_t)paging_info.minimum_leaf_size - 1u);
	paging_info.leaf_size_mask =
		(1ull << minimum_leaf_shift) | (1ull << (minimum_leaf_shift + 9u)) | (1ull << (minimum_leaf_shift + 18u));
	device_memory_supported = ((aarch64_read_mair_el1() >> 16u) & 0xffu) == AARCH64_MAIR_DEVICE_NGNRNE;

	if (!aarch64_query_entry((uintptr_t)&hal_paging_init, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	(void)shift;

	normal_attrs_template = entry & (AARCH64_LOWER_MASK | AARCH64_UPPER_MASK);
	kernel_space          = (struct hal_paging_space){
				 .lower_root_phys = (uintptr_t)(aarch64_read_ttbr0_el1() & pte_address_mask),
				 .upper_root_phys = (uintptr_t)(aarch64_read_ttbr1_el1() & pte_address_mask),
				 .flags           = 0u,
    };
	initialized = true;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

static bool aarch64_protection_supported(uint64_t flags) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && (flags & HAL_PAGE_READ) != 0u;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return aarch64_protection_supported(flags) &&
	       (memory_type == MEMORY_TYPE_NORMAL || (memory_type == MEMORY_TYPE_DEVICE && device_memory_supported));
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

static void aarch64_free_page_table_children(uint64_t* table, unsigned level) {
	if (table == NULL || level == 0u) return;
	for (size_t index = 0u; index < 512u; index++) {
		uint64_t entry = table[index];
		if ((entry & (AARCH64_DESC_VALID | AARCH64_DESC_TABLE)) != (AARCH64_DESC_VALID | AARCH64_DESC_TABLE)) {
			continue;
		}

		uintptr_t child_phys = (uintptr_t)(entry & pte_address_mask);
		uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
		aarch64_free_page_table_children(child, level - 1u);
		(void)pmm_free_pages(child_phys, 1u);
	}
}

void hal_paging_space_destroy(struct hal_paging_space* space) {
	struct aarch64_walk_params params;
	struct irq_state           state;
	uintptr_t                  root_phys;
	uint64_t*                  root;

	if (space == NULL || space->lower_root_phys == 0u) return;
	root_phys = space->lower_root_phys & pte_address_mask;
	if (root_phys == kernel_space.lower_root_phys) return;
	state = spinlock_lock_irqsave(&paging_lock);
	if (aarch64_walk_params_for_space(space, 0u, &params)) {
		root = (uint64_t*)hhdm_phys_to_virt(root_phys);
		aarch64_free_page_table_children(root, params.levels - 1u);
	}
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
	aarch64_write_ttbr0_el1(space->lower_root_phys & pte_address_mask);
	aarch64_write_ttbr1_el1(space->upper_root_phys & pte_address_mask);
	aarch64_tlb_flush_all();
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool aarch64_split_leaf(uint64_t* slot, unsigned level, struct paging_transaction* transaction) {
	uint64_t  entry = *slot;
	uintptr_t child_phys;
	uint64_t* child;
	size_t    parent_size = aarch64_leaf_size(level);
	size_t    child_size  = aarch64_leaf_size(level - 1u);
	uint64_t  base        = entry & pte_address_mask & ~((uint64_t)parent_size - 1u);
	uint64_t  flags       = entry & ~pte_address_mask & ~AARCH64_DESC_TABLE;
	if (level == 0u || (entry & AARCH64_DESC_VALID) == 0u || (entry & AARCH64_DESC_TABLE) != 0u ||
	    !pmm_alloc_pages(1u, &child_phys))
		return false;
	child = (uint64_t*)hhdm_phys_to_virt(child_phys);
	memset(child, 0, PMM_PAGE_SIZE);
	for (size_t index = 0u; index < 512u; index++) {
		child[index] = (base + index * child_size) | flags;
		if (level - 1u == 0u) child[index] |= AARCH64_DESC_TABLE;
	}
	if (!paging_transaction_record(transaction, slot, child_phys)) {
		(void)pmm_free_pages(child_phys, 1u);
		return false;
	}
	aarch64_replace_descriptor(slot, (child_phys & pte_address_mask) | AARCH64_DESC_VALID | AARCH64_DESC_TABLE);
	return true;
}

static bool aarch64_table_empty(const uint64_t* table) {
	for (size_t index = 0u; index < 512u; index++) {
		if ((table[index] & AARCH64_DESC_VALID) != 0u) return false;
	}
	return true;
}

static bool aarch64_prepare_range(uint64_t* table, unsigned level, uintptr_t start, uintptr_t end,
                                  struct paging_transaction* transaction) {
	size_t span = aarch64_leaf_size(level);
	while (start < end) {
		size_t    index = (start >> (minimum_leaf_shift + 9u * level)) & 0x1ffu;
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t* slot  = &table[index];
		uint64_t  entry = *slot;
		if ((entry & AARCH64_DESC_VALID) != 0u && level > 0u) {
			if ((entry & AARCH64_DESC_TABLE) == 0u) {
				uintptr_t leaf_start = start & ~(span - 1u);
				if (start != leaf_start || next != leaf_start + span) {
					if (!aarch64_split_leaf(slot, level, transaction)) return false;
					entry = *slot;
				}
			}
			if ((entry & AARCH64_DESC_TABLE) != 0u &&
			    !aarch64_prepare_range(
					(uint64_t*)hhdm_phys_to_virt(entry & pte_address_mask), level - 1u, start, next, transaction))
				return false;
		}
		start = next;
	}
	return true;
}

static bool aarch64_change_range(uint64_t* table, unsigned level, uintptr_t start, uintptr_t end, bool protect,
                                 uint64_t flags, struct paging_transaction* transaction) {
	size_t span = aarch64_leaf_size(level);
	while (start < end) {
		size_t    index = (size_t)((start >> (minimum_leaf_shift + 9u * level)) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & AARCH64_DESC_VALID) != 0u) {
			bool leaf = level == 0u || (entry & AARCH64_DESC_TABLE) == 0u;
			if (leaf) {
				if (level == 0u && (entry & AARCH64_DESC_TABLE) == 0u) return false;
				if (!paging_transaction_record(transaction, &table[index], 0u)) return false;
				if (protect) aarch64_replace_descriptor(&table[index], aarch64_leaf_apply_protection(entry, flags));
				else table[index] = 0u;
			}
			else {
				if ((entry & AARCH64_DESC_TABLE) == 0u) return false;
				uintptr_t child_phys = (uintptr_t)(entry & pte_address_mask);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!aarch64_change_range(child, level - 1u, start, next, protect, flags, transaction)) return false;
				if (!protect && aarch64_table_empty(child)) {
					if (!paging_transaction_retire(transaction, &table[index], child_phys)) return false;
					table[index] = 0u;
				}
			}
		}
		start = next;
	}
	return true;
}

static bool aarch64_range_args(struct hal_paging_space* space, uintptr_t virt, size_t size, uintptr_t* out_end,
                               struct aarch64_walk_params* out_params) {
	uint64_t end;
	if (space == NULL || !initialized || size == 0u || (virt & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (size & (paging_info.minimum_leaf_size - 1u)) != 0u || add_overflow_u64(virt, size, &end) ||
	    !aarch64_walk_params_for_space(space, virt, out_params) ||
	    aarch64_is_upper_region(virt) != aarch64_is_upper_region((uintptr_t)end - 1u))
		return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	uintptr_t                  end;
	struct aarch64_walk_params params;
	if (!aarch64_range_args(space, virt, size, &end, &params)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uint64_t*                 root        = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);
	struct paging_transaction transaction = {0};
	bool                      ok          = aarch64_prepare_range(root, params.levels - 1u, virt, end, &transaction) &&
	          aarch64_change_range(root, params.levels - 1u, virt, end, false, 0u, &transaction);
	if (ok) {
		aarch64_tlb_flush_all();
		paging_transaction_commit(&transaction);
	}
	else {
		paging_transaction_rollback(&transaction, aarch64_transaction_restore, NULL);
		aarch64_tlb_flush_all();
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	uintptr_t                  end;
	struct aarch64_walk_params params;
	if (!aarch64_protection_supported(flags) || !aarch64_range_args(space, virt, size, &end, &params)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uint64_t*                 root        = (uint64_t*)hhdm_phys_to_virt((uintptr_t)params.root_phys);
	struct paging_transaction transaction = {0};
	bool                      ok          = aarch64_prepare_range(root, params.levels - 1u, virt, end, &transaction) &&
	          aarch64_change_range(root, params.levels - 1u, virt, end, true, flags, &transaction);
	aarch64_tlb_shootdown_range(virt, size);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, aarch64_transaction_restore, NULL);
		aarch64_tlb_flush_all();
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
	uint64_t         page_mask;
	struct irq_state state;

	if (out_translation) *out_translation = (struct hal_paging_translation){0};

	if (space == NULL) return false;
	if (!initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!aarch64_query_entry_in(space, virt, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	page_mask                  = (1ull << shift) - 1u;
	uintptr_t physical_address = (uintptr_t)((entry & pte_address_mask & ~page_mask) | ((uint64_t)virt & page_mask));
	flags |= HAL_PAGE_READ;

	if ((entry & AARCH64_AP_MASK) == AARCH64_AP_RW_EL1 || (entry & AARCH64_AP_MASK) == AARCH64_AP_RW_EL0) {
		flags |= HAL_PAGE_WRITE;
	}
	if ((entry & AARCH64_AP_MASK) == AARCH64_AP_RW_EL0 || (entry & AARCH64_AP_MASK) == AARCH64_AP_RO_EL0) {
		flags |= HAL_PAGE_USER;
	}
	if ((flags & HAL_PAGE_USER) != 0) {
		if ((entry & AARCH64_UXN) == 0) flags |= HAL_PAGE_EXEC;
	}
	else if ((entry & AARCH64_PXN) == 0) {
		flags |= HAL_PAGE_EXEC;
	}
	if ((entry & AARCH64_NG) == 0) flags |= HAL_PAGE_GLOBAL;
	if (out_translation)
		*out_translation = (struct hal_paging_translation){
			physical_address, (size_t)1u << shift, flags, aarch64_leaf_memory_type(entry)};

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	struct aarch64_walk_params params;
	uint64_t                   end;
	if (space == NULL || request == NULL || !initialized ||
	    !hal_paging_mapping_supported(request->flags, request->memory_type) || request->size == 0u ||
	    (request->virtual_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->physical_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->size & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address ||
	    ((request->physical_address + request->size - 1u) &
	     ~(pte_address_mask | ((uint64_t)paging_info.minimum_leaf_size - 1u))) != 0u ||
	    add_overflow_u64(request->virtual_address, request->size, &end) ||
	    !aarch64_walk_params_for_space(space, request->virtual_address, &params) ||
	    aarch64_is_upper_region(request->virtual_address) != aarch64_is_upper_region((uintptr_t)end - 1u))
		return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	struct paging_transaction transaction = {0};
	size_t                    mapped      = 0u;
	bool                      ok          = true;
	while (mapped < request->size) {
		uintptr_t virt      = request->virtual_address + mapped;
		uintptr_t phys      = request->physical_address + mapped;
		size_t    remaining = request->size - mapped;
		unsigned  level     = params.levels - 1u;
		if (level > 2u) level = 2u;
		while (level > 0u) {
			size_t leaf_size = aarch64_leaf_size(level);
			if ((virt & (leaf_size - 1u)) == 0u && (phys & (leaf_size - 1u)) == 0u && remaining >= leaf_size) break;
			level--;
		}
		uint64_t* slot;
		if (!aarch64_walk_to_level(space, virt, level, &transaction, &slot) || (*slot & AARCH64_DESC_VALID) != 0u ||
		    !paging_transaction_record(&transaction, slot, 0u)) {
			ok = false;
			break;
		}
		*slot = ((uint64_t)phys & pte_address_mask & ~((uint64_t)aarch64_leaf_size(level) - 1u)) |
		        aarch64_leaf_flags(request->flags, request->memory_type);
		if (level == 0u) *slot |= AARCH64_DESC_TABLE;
		mapped += aarch64_leaf_size(level);
	}
	aarch64_tlb_shootdown_range(request->virtual_address, request->size);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, aarch64_transaction_restore, NULL);
		aarch64_tlb_flush_all();
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
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
