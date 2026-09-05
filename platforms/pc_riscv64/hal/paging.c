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

#define RISCV_PTE_V (1ull << 0)
#define RISCV_PTE_R (1ull << 1)
#define RISCV_PTE_W (1ull << 2)
#define RISCV_PTE_X (1ull << 3)
#define RISCV_PTE_U (1ull << 4)
#define RISCV_PTE_G (1ull << 5)
#define RISCV_PTE_A (1ull << 6)
#define RISCV_PTE_D (1ull << 7)
#define RISCV_PTE_PBMT_MASK (3ull << 61)
#define RISCV_PTE_PBMT_IO (2ull << 61)
#define RISCV_PTE_PPN_MASK 0x003ffffffffffc00ull

#define RISCV_SATP_MODE_SV39 8ull
#define RISCV_SATP_MODE_SV48 9ull
#define RISCV_SATP_MODE_SV57 10ull

#define RISCV_SBI_EID_RFENCE 0x52464e43ul
#define RISCV_SBI_FID_REMOTE_SFENCE_VMA 1ul

struct hal_paging_space {
	uintptr_t lower_root_phys;
	uintptr_t upper_root_phys;
	uint64_t  flags;
	uintptr_t storage_phys;
};

static struct hal_paging_info paging_info;

struct riscv_sbi_ret {
	long error;
	long value;
};

static bool                    initialized;
static int                     paging_levels;
static unsigned                minimum_leaf_shift;
static bool                    svpbmt_available;
static uint64_t                satp_prefix;
static struct hal_paging_space kernel_space;
static struct spinlock         paging_lock =
	SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static inline uint64_t riscv_read_satp(void) {
	uint64_t value;
	__asm__ volatile("csrr %0, satp" : "=r"(value));
	return value;
}

static inline void riscv_write_satp(uint64_t value) {
	__asm__ volatile("csrw satp, %0\n\tsfence.vma x0, x0" : : "r"(value) : "memory");
}

static inline uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + boot_info.direct_map_offset;
}

static inline uintptr_t riscv_pte_to_phys(uint64_t pte) {
	return (uintptr_t)((pte & RISCV_PTE_PPN_MASK) << (minimum_leaf_shift - 10u));
}

static inline uint64_t riscv_pte_from_phys(uintptr_t phys) {
	return (((uint64_t)phys >> minimum_leaf_shift) << 10u) & RISCV_PTE_PPN_MASK;
}

static inline size_t riscv_leaf_size(int level) {
	return (size_t)1u << (minimum_leaf_shift + 9u * (unsigned)level);
}

static inline bool riscv_virtual_address_valid(uintptr_t virt) {
	unsigned bits     = minimum_leaf_shift + 9u * (unsigned)paging_levels;
	uint64_t sign     = ((uint64_t)virt >> (bits - 1u)) & 1u;
	uint64_t upper    = (uint64_t)virt >> bits;
	uint64_t expected = sign != 0u ? ((1ull << (64u - bits)) - 1u) : 0u;
	return upper == expected;
}

static inline void riscv_tlb_flush(uintptr_t virt) {
	__asm__ volatile("sfence.vma %0, x0" : : "r"(virt) : "memory");
}

static struct riscv_sbi_ret riscv_sbi_call4(unsigned long arg0, unsigned long arg1, unsigned long arg2,
                                            unsigned long arg3, unsigned long fid, unsigned long eid) {
	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	register unsigned long a2 asm("a2") = arg2;
	register unsigned long a3 asm("a3") = arg3;
	register unsigned long a6 asm("a6") = fid;
	register unsigned long a7 asm("a7") = eid;

	__asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a6), "r"(a7) : "memory", "a4", "a5");
	return (struct riscv_sbi_ret){
		.error = (long)a0,
		.value = (long)a1,
	};
}

static void riscv_tlb_shootdown(uintptr_t start, size_t size, bool hierarchy_changed) {
	const struct cpu_topology* topology = cpu_topology_get();
	struct cpu*                current  = cpu_current();

	if (size == 0u) return;
	if (hierarchy_changed) {
		__asm__ volatile("sfence.vma x0, x0" ::: "memory");
	}
	else {
		for (size_t offset = 0u; offset < size; offset += paging_info.minimum_leaf_size) {
			riscv_tlb_flush(start + offset);
		}
	}

	if (topology == NULL || topology->cpus == NULL || current == NULL || topology->cpu_count == 0u) {
		hcf();
	}
	/* Publish page-table stores before firmware asks remote harts to fence. */
	__asm__ volatile("fence rw, rw" : : : "memory");

	for (size_t i = 0u; i < topology->cpu_count; i++) {
		struct cpu*          target = &topology->cpus[i];
		struct riscv_sbi_ret ret;

		if (target == current || cpu_state_get(target) != CPU_STATE_ONLINE) continue;

		/* SBI RFENCE is synchronous: successful return means the selected hart
		 * has completed the requested SFENCE.VMA before execution continues here. */
		ret = riscv_sbi_call4(1ul,
		                      (unsigned long)target->arch_id,
		                      hierarchy_changed ? 0ul : (unsigned long)start,
		                      hierarchy_changed ? 0ul : (unsigned long)size,
		                      RISCV_SBI_FID_REMOTE_SFENCE_VMA,
		                      RISCV_SBI_EID_RFENCE);
		if (ret.error != 0) hcf();
	}
}

static inline uint64_t riscv_leaf_flags(uint64_t flags, enum memory_type memory_type) {
	uint64_t entry = RISCV_PTE_V | RISCV_PTE_A;

	if ((flags & HAL_PAGE_READ) != 0) entry |= RISCV_PTE_R;
	if ((flags & HAL_PAGE_WRITE) != 0) entry |= RISCV_PTE_W | RISCV_PTE_D;
	if ((flags & HAL_PAGE_EXEC) != 0) entry |= RISCV_PTE_X;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= RISCV_PTE_G;
	if ((flags & HAL_PAGE_USER) != 0) entry |= RISCV_PTE_U;
	if (memory_type == MEMORY_TYPE_DEVICE) entry |= RISCV_PTE_PBMT_IO;

	return entry;
}

static inline uint64_t riscv_leaf_apply_protection(uint64_t entry, uint64_t flags) {
	entry &= ~(RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X | RISCV_PTE_U | RISCV_PTE_G);
	if ((flags & HAL_PAGE_READ) != 0u) entry |= RISCV_PTE_R;
	if ((flags & HAL_PAGE_WRITE) != 0u) entry |= RISCV_PTE_W | RISCV_PTE_D;
	if ((flags & HAL_PAGE_EXEC) != 0u) entry |= RISCV_PTE_X;
	if ((flags & HAL_PAGE_USER) != 0u) entry |= RISCV_PTE_U;
	if ((flags & HAL_PAGE_GLOBAL) != 0u) entry |= RISCV_PTE_G;
	return entry;
}

static void riscv_transaction_restore(uint64_t* slot, uint64_t previous, void* context) {
	(void)context;
	*slot = previous;
}

static inline uint64_t* riscv_root_table(void) {
	uint64_t  satp      = riscv_read_satp();
	uintptr_t root_phys = (uintptr_t)((satp & ((1ull << 44) - 1u)) << minimum_leaf_shift);

	if (root_phys == 0) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(root_phys);
}

static inline uintptr_t riscv_current_root_phys(void) {
	return (uintptr_t)((riscv_read_satp() & ((1ull << 44) - 1u)) << minimum_leaf_shift);
}

static inline uint64_t* riscv_space_root_table(const struct hal_paging_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(space->lower_root_phys);
}

static bool riscv_walk_to_level(const struct hal_paging_space* space, uintptr_t virt, int target_level,
                                struct paging_transaction* transaction, uint64_t** out_slot) {
	uint64_t* table = riscv_space_root_table(space);

	if (!table || !out_slot || target_level < 0 || target_level >= paging_levels) return false;

	for (int level = paging_levels - 1; level > target_level; level--) {
		size_t   index = (size_t)((virt >> (minimum_leaf_shift + 9u * (unsigned)level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & RISCV_PTE_V) == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			if (!paging_transaction_record(transaction, &table[index], next_phys)) {
				(void)pmm_free_pages(next_phys, 1u);
				return false;
			}
			table[index] = riscv_pte_from_phys(next_phys) | RISCV_PTE_V;
			entry        = table[index];
		}
		else if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry));
	}

	*out_slot = &table[(virt >> (minimum_leaf_shift + 9u * (unsigned)target_level)) & 0x1ffu];
	return true;
}

bool hal_paging_init(void) {
	uint64_t         satp  = riscv_read_satp();
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);

	switch (satp >> 60) {
	case RISCV_SATP_MODE_SV39:
		paging_levels = 3;
		break;
	case RISCV_SATP_MODE_SV48:
		paging_levels = 4;
		break;
	case RISCV_SATP_MODE_SV57:
		paging_levels = 5;
		break;
	default:
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	minimum_leaf_shift            = 12u;
	paging_info.minimum_leaf_size = (size_t)1u << minimum_leaf_shift;
	paging_info.leaf_size_mask    = 0u;
	for (int level = 0; level < paging_levels; level++)
		paging_info.leaf_size_mask |= 1ull << (minimum_leaf_shift + 9u * (unsigned)level);
	/* S-mode cannot read menvcfg.PBMTE, and SBI exposes no architectural query for it.
	 * A DT ISA declaration
	 * alone therefore cannot prove that PBMT is enabled for satp. */
	svpbmt_available = false;

	satp_prefix  = satp & ~((1ull << 44) - 1u);
	kernel_space = (struct hal_paging_space){
		.lower_root_phys = riscv_current_root_phys(),
		.upper_root_phys = 0u,
		.flags           = satp_prefix,
	};
	initialized = riscv_root_table() != NULL;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return initialized;
}

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

static bool riscv_query_entry(const struct hal_paging_space* space, uintptr_t virt, uint64_t* out_entry,
                              unsigned* out_shift) {
	uint64_t* table = riscv_space_root_table(space);
	if (table == NULL || out_entry == NULL || out_shift == NULL || !riscv_virtual_address_valid(virt)) return false;
	for (int level = paging_levels - 1; level >= 0; level--) {
		uint64_t entry = table[(virt >> (minimum_leaf_shift + 9u * (unsigned)level)) & 0x1ffu];
		if ((entry & RISCV_PTE_V) == 0u) return false;
		if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u) {
			*out_entry = entry;
			*out_shift = minimum_leaf_shift + 9u * (unsigned)level;
			return true;
		}
		if (level == 0) return false;
		table = (uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry));
	}
	return false;
}

static bool riscv_protection_supported(uint64_t flags) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && (flags & (HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_EXEC)) != 0u &&
	       ((flags & HAL_PAGE_WRITE) == 0u || (flags & HAL_PAGE_READ) != 0u);
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return riscv_protection_supported(flags) &&
	       (memory_type == MEMORY_TYPE_NORMAL || (memory_type == MEMORY_TYPE_DEVICE && svpbmt_available)) &&
	       memory_type < MEMORY_TYPE_COUNT;
}

struct hal_paging_space* hal_paging_kernel_space(void) {
	return initialized ? &kernel_space : NULL;
}

bool hal_paging_space_create(struct hal_paging_space** out_space) {
	uintptr_t                root_phys    = 0;
	uintptr_t                storage_phys = 0;
	struct hal_paging_space* space;
	uint64_t*                root;
	uint64_t*                kernel_root;
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

	root        = (uint64_t*)hhdm_phys_to_virt(root_phys);
	kernel_root = riscv_space_root_table(&kernel_space);
	if (kernel_root == NULL) {
		(void)pmm_free_pages(root_phys, 1);
		(void)pmm_free_pages(storage_phys, 1);
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	memset(root, 0, PMM_PAGE_SIZE);
	for (size_t index = 256u; index < 512u; index++) {
		root[index] = kernel_root[index];
	}
	*space = (struct hal_paging_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = 0u,
		.flags           = satp_prefix,
		.storage_phys    = storage_phys,
	};
	*out_space = space;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static void riscv_free_page_table_children(uint64_t* table, int level, size_t entry_count) {
	if (table == NULL || level <= 0) return;
	for (size_t index = 0u; index < entry_count; index++) {
		uint64_t entry = table[index];
		if ((entry & RISCV_PTE_V) == 0u || (entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u) continue;

		uintptr_t child_phys = riscv_pte_to_phys(entry);
		uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
		riscv_free_page_table_children(child, level - 1, 512u);
		(void)pmm_free_pages(child_phys, 1u);
	}
}

void hal_paging_space_destroy(struct hal_paging_space* space) {
	struct irq_state state;
	uintptr_t        root_phys;
	uint64_t*        root;

	if (space == NULL || space->lower_root_phys == 0u) return;
	root_phys = space->lower_root_phys;
	if (root_phys == kernel_space.lower_root_phys) return;
	state = spinlock_lock_irqsave(&paging_lock);
	root  = (uint64_t*)hhdm_phys_to_virt(root_phys);
	riscv_free_page_table_children(root, paging_levels - 1, 256u);
	(void)pmm_free_pages(root_phys, 1u);
	uintptr_t storage_phys = space->storage_phys;
	*space                 = (struct hal_paging_space){0};
	if (storage_phys != 0u) (void)pmm_free_pages(storage_phys, 1u);
	spinlock_unlock_irqrestore(&paging_lock, state);
}

bool hal_paging_activate(const struct hal_paging_space* space) {
	struct irq_state state;

	if (space == NULL || space->lower_root_phys == 0u || !initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	riscv_write_satp(satp_prefix | ((uint64_t)space->lower_root_phys >> minimum_leaf_shift));
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static inline bool riscv_is_leaf(uint64_t entry) {
	return (entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u;
}

static bool riscv_split_leaf(uint64_t* slot, int level, struct paging_transaction* transaction) {
	uint64_t  entry = *slot;
	uintptr_t child_phys;
	uint64_t* child;
	size_t    parent_size = riscv_leaf_size(level);
	size_t    child_size  = riscv_leaf_size(level - 1);
	uintptr_t base        = riscv_pte_to_phys(entry) & ~(parent_size - 1u);
	uint64_t  flags       = entry & ~RISCV_PTE_PPN_MASK;
	if (level <= 0 || !riscv_is_leaf(entry) || !pmm_alloc_pages(1u, &child_phys)) return false;
	child = (uint64_t*)hhdm_phys_to_virt(child_phys);
	memset(child, 0, PMM_PAGE_SIZE);
	for (size_t index = 0u; index < 512u; index++)
		child[index] = riscv_pte_from_phys(base + index * child_size) | flags;
	if (!paging_transaction_record(transaction, slot, child_phys)) {
		(void)pmm_free_pages(child_phys, 1u);
		return false;
	}
	*slot = riscv_pte_from_phys(child_phys) | RISCV_PTE_V;
	return true;
}

static bool riscv_table_empty(const uint64_t* table) {
	for (size_t index = 0u; index < 512u; index++) {
		if ((table[index] & RISCV_PTE_V) != 0u) return false;
	}
	return true;
}

static bool riscv_prepare_range(uint64_t* table, int level, uintptr_t start, uintptr_t end,
                                struct paging_transaction* transaction) {
	size_t span = riscv_leaf_size(level);
	while (start < end) {
		size_t    index = (start >> (minimum_leaf_shift + 9u * (unsigned)level)) & 0x1ffu;
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t* slot  = &table[index];
		uint64_t  entry = *slot;
		if ((entry & RISCV_PTE_V) != 0u && level > 0) {
			if (riscv_is_leaf(entry)) {
				uintptr_t leaf_start = start & ~(span - 1u);
				if (start != leaf_start || next != leaf_start + span) {
					if (!riscv_split_leaf(slot, level, transaction)) return false;
					entry = *slot;
				}
			}
			if (!riscv_is_leaf(entry) &&
			    !riscv_prepare_range(
					(uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry)), level - 1, start, next, transaction))
				return false;
		}
		start = next;
	}
	return true;
}

static bool riscv_change_range(uint64_t* table, int level, uintptr_t start, uintptr_t end, bool protect, uint64_t flags,
                               struct paging_transaction* transaction) {
	size_t span = riscv_leaf_size(level);
	while (start < end) {
		size_t    index = (size_t)((start >> (minimum_leaf_shift + 9u * (unsigned)level)) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & RISCV_PTE_V) != 0u) {
			bool leaf = (entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u;
			if (leaf) {
				if (!paging_transaction_record(transaction, &table[index], 0u)) return false;
				if (protect) table[index] = riscv_leaf_apply_protection(entry, flags);
				else table[index] = 0u;
			}
			else {
				if (level == 0) return false;
				uintptr_t child_phys = riscv_pte_to_phys(entry);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!riscv_change_range(child, level - 1, start, next, protect, flags, transaction)) return false;
				if (!protect && riscv_table_empty(child)) {
					if (!paging_transaction_retire(transaction, &table[index], child_phys)) return false;
					table[index] = 0u;
				}
			}
		}
		start = next;
	}
	return true;
}

static bool riscv_range_args(struct hal_paging_space* space, uintptr_t virt, size_t size, uintptr_t* out_end) {
	uint64_t end;
	if (space == NULL || !initialized || size == 0u || !riscv_virtual_address_valid(virt) ||
	    (virt & (paging_info.minimum_leaf_size - 1u)) != 0u || (size & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    add_overflow_u64(virt, size, &end))
		return false;
	if (!riscv_virtual_address_valid((uintptr_t)end - 1u)) return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	uintptr_t end;
	if (!riscv_range_args(space, virt, size, &end)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uint64_t*                 root        = riscv_space_root_table(space);
	struct paging_transaction transaction = {0};
	bool ok = root != NULL && riscv_prepare_range(root, paging_levels - 1, virt, end, &transaction) &&
	          riscv_change_range(root, paging_levels - 1, virt, end, false, 0u, &transaction);
	riscv_tlb_shootdown(virt, size, transaction.hierarchy_changed);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, riscv_transaction_restore, NULL);
		riscv_tlb_shootdown(virt, size, transaction.hierarchy_changed);
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	uintptr_t end;
	if (!riscv_protection_supported(flags) || !riscv_range_args(space, virt, size, &end)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uint64_t*                 root        = riscv_space_root_table(space);
	struct paging_transaction transaction = {0};
	bool ok = root != NULL && riscv_prepare_range(root, paging_levels - 1, virt, end, &transaction) &&
	          riscv_change_range(root, paging_levels - 1, virt, end, true, flags, &transaction);
	riscv_tlb_shootdown(virt, size, transaction.hierarchy_changed);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, riscv_transaction_restore, NULL);
		riscv_tlb_shootdown(virt, size, transaction.hierarchy_changed);
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
	if (!riscv_query_entry(space, virt, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	uint64_t  leaf_mask        = ((uint64_t)1u << shift) - 1u;
	uintptr_t physical_address = (riscv_pte_to_phys(entry) & ~leaf_mask) | (virt & leaf_mask);
	if ((entry & RISCV_PTE_R) != 0) flags |= HAL_PAGE_READ;
	if ((entry & RISCV_PTE_W) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & RISCV_PTE_X) != 0) flags |= HAL_PAGE_EXEC;
	if ((entry & RISCV_PTE_G) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & RISCV_PTE_U) != 0) flags |= HAL_PAGE_USER;
	if (out_translation)
		*out_translation = (struct hal_paging_translation){
			.physical_address = physical_address,
			.leaf_size        = (size_t)1u << shift,
			.flags            = flags,
			.memory_type = (entry & RISCV_PTE_PBMT_MASK) == RISCV_PTE_PBMT_IO ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL,
		};

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool riscv_remap_range(uint64_t* table, int level, uintptr_t start, uintptr_t end, uintptr_t request_start,
                              uintptr_t physical_start, struct paging_transaction* transaction) {
	size_t span = riscv_leaf_size(level);
	while (start < end) {
		size_t    index = (size_t)((start >> (minimum_leaf_shift + 9u * (unsigned)level)) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t* slot  = &table[index];
		uint64_t  entry = *slot;
		uintptr_t phys  = physical_start + (start - request_start);

		if ((entry & RISCV_PTE_V) == 0u) return false;
		if (riscv_is_leaf(entry)) {
			uintptr_t leaf_start = start & ~((uintptr_t)span - 1u);
			if (level > 0 && (start != leaf_start || next != leaf_start + span || (phys & (span - 1u)) != 0u)) {
				if (!riscv_split_leaf(slot, level, transaction) ||
				    !riscv_remap_range((uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(*slot)),
				                       level - 1,
				                       start,
				                       next,
				                       request_start,
				                       physical_start,
				                       transaction))
					return false;
			}
			else {
				if (!paging_transaction_record(transaction, slot, 0u)) return false;
				*slot = (entry & ~RISCV_PTE_PPN_MASK) | riscv_pte_from_phys(phys & ~(span - 1u));
			}
		}
		else {
			if (level == 0 || !riscv_remap_range((uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry)),
			                                     level - 1,
			                                     start,
			                                     next,
			                                     request_start,
			                                     physical_start,
			                                     transaction))
				return false;
		}
		start = next;
	}
	return true;
}

bool hal_paging_remap(struct hal_paging_space* space, const struct hal_paging_remap_request* request) {
	uintptr_t end;
	if (request == NULL || !riscv_range_args(space, request->virtual_address, request->size, &end) ||
	    (request->physical_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    request->size > UINTPTR_MAX - request->physical_address ||
	    ((request->physical_address + request->size - 1u) &
	     ~((RISCV_PTE_PPN_MASK << (minimum_leaf_shift - 10u)) | ((uint64_t)paging_info.minimum_leaf_size - 1u))) != 0u)
		return false;

	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	uint64_t*                 root        = riscv_space_root_table(space);
	struct paging_transaction transaction = {0};
	bool                      ok          = root != NULL && riscv_remap_range(root,
                                                paging_levels - 1,
                                                request->virtual_address,
                                                end,
                                                request->virtual_address,
                                                request->physical_address,
                                                &transaction);
	riscv_tlb_shootdown(request->virtual_address, request->size, transaction.hierarchy_changed);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, riscv_transaction_restore, NULL);
		riscv_tlb_shootdown(request->virtual_address, request->size, transaction.hierarchy_changed);
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	if (space == NULL || request == NULL || !initialized ||
	    !hal_paging_mapping_supported(request->flags, request->memory_type) || request->size == 0u ||
	    (request->virtual_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->physical_address & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    (request->size & (paging_info.minimum_leaf_size - 1u)) != 0u ||
	    !riscv_virtual_address_valid(request->virtual_address) ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address ||
	    ((request->physical_address + request->size - 1u) &
	     ~((RISCV_PTE_PPN_MASK << (minimum_leaf_shift - 10u)) | ((uint64_t)paging_info.minimum_leaf_size - 1u))) != 0u)
		return false;
	if (!riscv_virtual_address_valid(request->virtual_address + request->size - 1u)) return false;
	struct irq_state          state       = spinlock_lock_irqsave(&paging_lock);
	struct paging_transaction transaction = {0};
	size_t                    mapped      = 0u;
	bool                      ok          = true;
	while (mapped < request->size) {
		uintptr_t virt      = request->virtual_address + mapped;
		uintptr_t phys      = request->physical_address + mapped;
		size_t    remaining = request->size - mapped;
		int       level     = paging_levels - 1;
		while (level > 0) {
			size_t leaf_size = riscv_leaf_size(level);
			if ((virt & (leaf_size - 1u)) == 0u && (phys & (leaf_size - 1u)) == 0u && remaining >= leaf_size) break;
			level--;
		}
		uint64_t* slot;
		if (!riscv_walk_to_level(space, virt, level, &transaction, &slot) || (*slot & RISCV_PTE_V) != 0u ||
		    !paging_transaction_record(&transaction, slot, 0u)) {
			ok = false;
			break;
		}
		*slot = riscv_pte_from_phys(phys & ~(riscv_leaf_size(level) - 1u)) |
		        riscv_leaf_flags(request->flags, request->memory_type);
		mapped += riscv_leaf_size(level);
	}
	riscv_tlb_shootdown(request->virtual_address, request->size, transaction.hierarchy_changed);
	if (ok) paging_transaction_commit(&transaction);
	else {
		paging_transaction_rollback(&transaction, riscv_transaction_restore, NULL);
		riscv_tlb_shootdown(request->virtual_address, request->size, transaction.hierarchy_changed);
		paging_transaction_abort(&transaction);
	}
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}
