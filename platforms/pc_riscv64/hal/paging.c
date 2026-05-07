#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RISCV_PTE_V (1ull << 0)
#define RISCV_PTE_R (1ull << 1)
#define RISCV_PTE_W (1ull << 2)
#define RISCV_PTE_X (1ull << 3)
#define RISCV_PTE_U (1ull << 4)
#define RISCV_PTE_G (1ull << 5)
#define RISCV_PTE_A (1ull << 6)
#define RISCV_PTE_D (1ull << 7)

#define RISCV_SATP_MODE_SV39 8ull
#define RISCV_SATP_MODE_SV48 9ull
#define RISCV_SATP_MODE_SV57 10ull

static bool                     initialized;
static int                      paging_levels;
static uint64_t                 satp_prefix;
static struct hal_address_space kernel_space;
static struct spinlock          paging_lock =
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
	return (uintptr_t)((pte >> 10) << 12);
}

static inline uint64_t riscv_pte_from_phys(uintptr_t phys) {
	return ((uint64_t)phys >> 12) << 10;
}

static inline void riscv_tlb_flush(uintptr_t virt) {
	__asm__ volatile("sfence.vma %0, x0" : : "r"(virt) : "memory");
}

static inline uint64_t riscv_leaf_flags(uint64_t flags) {
	uint64_t entry = RISCV_PTE_V | RISCV_PTE_R | RISCV_PTE_A;

	if ((flags & HAL_PAGE_WRITE) != 0) entry |= RISCV_PTE_W | RISCV_PTE_D;
	if ((flags & HAL_PAGE_EXEC) != 0) entry |= RISCV_PTE_X;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= RISCV_PTE_G;
	if ((flags & HAL_PAGE_USER) != 0) entry |= RISCV_PTE_U;

	return entry;
}

static inline uint64_t* riscv_root_table(void) {
	uint64_t  satp      = riscv_read_satp();
	uintptr_t root_phys = (uintptr_t)((satp & ((1ull << 44) - 1u)) << 12);

	if (root_phys == 0) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(root_phys);
}

static inline uintptr_t riscv_current_root_phys(void) {
	return (uintptr_t)((riscv_read_satp() & ((1ull << 44) - 1u)) << 12);
}

static inline uint64_t* riscv_space_root_table(const struct hal_address_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(space->lower_root_phys);
}

static bool riscv_walk_to_leaf_in(const struct hal_address_space* space, uintptr_t virt, bool create,
                                  uint64_t** out_table, size_t* out_index) {
	uint64_t* table = riscv_space_root_table(space);

	if (!table || !out_table || !out_index) return false;

	for (int level = paging_levels - 1; level > 0; level--) {
		size_t   index = (size_t)((virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & RISCV_PTE_V) == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!create) return false;
			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			table[index] = riscv_pte_from_phys(next_phys) | RISCV_PTE_V;
			entry        = table[index];
		}
		else if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry));
	}

	*out_table = table;
	*out_index = (size_t)((virt >> 12) & 0x1ffu);
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

	satp_prefix  = satp & ~((1ull << 44) - 1u);
	kernel_space = (struct hal_address_space){
		.lower_root_phys = riscv_current_root_phys(),
		.upper_root_phys = 0u,
		.flags           = satp_prefix,
	};
	initialized = riscv_root_table() != NULL;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return initialized;
}

struct hal_address_space* hal_paging_kernel_space(void) {
	return initialized ? &kernel_space : NULL;
}

bool hal_paging_space_create(struct hal_address_space* out_space) {
	uintptr_t        root_phys = 0;
	uint64_t*        root;
	uint64_t*        kernel_root;
	struct irq_state state;

	if (out_space == NULL || !initialized) return false;

	state = spinlock_lock_irqsave(&paging_lock);
	if (!pmm_alloc_pages(1, &root_phys)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	root        = (uint64_t*)hhdm_phys_to_virt(root_phys);
	kernel_root = riscv_space_root_table(&kernel_space);
	if (kernel_root == NULL) {
		(void)pmm_free_pages(root_phys, 1);
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	memset(root, 0, PMM_PAGE_SIZE);
	for (size_t index = 256u; index < 512u; index++) {
		root[index] = kernel_root[index];
	}
	*out_space = (struct hal_address_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = 0u,
		.flags           = satp_prefix,
	};
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

void hal_paging_space_destroy(struct hal_address_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return;
	if (space->lower_root_phys != kernel_space.lower_root_phys) {
		(void)pmm_free_pages(space->lower_root_phys, 1);
	}
	*space = (struct hal_address_space){0};
}

bool hal_paging_activate(const struct hal_address_space* space) {
	struct irq_state state;

	if (space == NULL || space->lower_root_phys == 0u || !initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	riscv_write_satp(satp_prefix | ((uint64_t)space->lower_root_phys >> 12));
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_map(struct hal_address_space* space, uintptr_t virt, uintptr_t phys, uint64_t flags) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	struct irq_state state;

	if (space == NULL) return false;
	if (!initialized) return false;
	if ((flags & ~HAL_PAGE_VALID_MASK) != 0) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!riscv_walk_to_leaf_in(space, virt, true, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & RISCV_PTE_V) != 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = riscv_pte_from_phys(phys) | riscv_leaf_flags(flags);
	riscv_tlb_flush(virt);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_unmap(struct hal_address_space* space, uintptr_t virt) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	struct irq_state state;

	if (space == NULL) return false;
	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!riscv_walk_to_leaf_in(space, virt, false, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & RISCV_PTE_V) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = 0;
	riscv_tlb_flush(virt);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_query(const struct hal_address_space* space, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	uint64_t         entry;
	uint64_t         flags = 0;
	struct irq_state state;

	if (out_phys) *out_phys = 0;
	if (out_flags) *out_flags = 0;

	if (space == NULL) return false;
	if (!initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!riscv_walk_to_leaf_in(space, virt, false, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	entry = table[index];
	if ((entry & RISCV_PTE_V) == 0 || (entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	if (out_phys) *out_phys = riscv_pte_to_phys(entry) | (virt & (PMM_PAGE_SIZE - 1u));
	if ((entry & RISCV_PTE_W) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & RISCV_PTE_X) != 0) flags |= HAL_PAGE_EXEC;
	if ((entry & RISCV_PTE_G) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & RISCV_PTE_U) != 0) flags |= HAL_PAGE_USER;
	if (out_flags) *out_flags = flags;

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	__asm__ volatile("fence.i" : : : "memory");
}
