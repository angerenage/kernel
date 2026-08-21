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

#define X86_PTE_PRESENT (1ull << 0)
#define X86_PTE_WRITE (1ull << 1)
#define X86_PTE_USER (1ull << 2)
#define X86_PTE_PWT (1ull << 3)
#define X86_PTE_PCD (1ull << 4)
#define X86_PTE_LARGE (1ull << 7)
#define X86_PTE_GLOBAL (1ull << 8)
#define X86_PTE_NX (1ull << 63)
#define X86_CR4_LA57 (1ull << 12)
#define X86_PHYS_MASK 0x000ffffffffff000ull

static bool                     initialized;
static struct hal_address_space kernel_space;
static struct spinlock          paging_lock =
	SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static inline uint64_t x86_read_cr3(void) {
	uint64_t value;
	__asm__ volatile("mov %%cr3, %0" : "=r"(value));
	return value;
}

static inline uint64_t x86_read_cr4(void) {
	uint64_t value;
	__asm__ volatile("mov %%cr4, %0" : "=r"(value));
	return value;
}

static inline void x86_write_cr3(uint64_t value) {
	__asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void x86_invlpg(uintptr_t virt) {
	__asm__ volatile("invlpg (%0)" : : "r"((void*)virt) : "memory");
}

static inline uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + boot_info.direct_map_offset;
}

static inline uint64_t x86_leaf_flags(uint64_t flags) {
	uint64_t entry = X86_PTE_PRESENT;

	if ((flags & HAL_PAGE_WRITE) != 0) entry |= X86_PTE_WRITE;
	if ((flags & HAL_PAGE_EXEC) == 0) entry |= X86_PTE_NX;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= X86_PTE_GLOBAL;
	if ((flags & HAL_PAGE_NO_CACHE) != 0) entry |= X86_PTE_PCD;
	if ((flags & HAL_PAGE_USER) != 0) entry |= X86_PTE_USER;

	return entry;
}

static inline uint64_t* x86_space_root_table(const struct hal_address_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(space->lower_root_phys & X86_PHYS_MASK);
}

static inline int x86_paging_levels(void) {
	return (x86_read_cr4() & X86_CR4_LA57) != 0 ? 5 : 4;
}

static bool x86_walk_to_leaf(const struct hal_address_space* space, uintptr_t virt, bool create, bool user,
                             uint64_t** out_table, size_t* out_index) {
	uint64_t* table  = x86_space_root_table(space);
	int       levels = x86_paging_levels();

	if (!table || !out_table || !out_index) return false;

	for (int level = levels - 1; level > 0; level--) {
		size_t   index = (size_t)((virt >> (12 + 9 * level)) & 0x1ffu);
		uint64_t entry = table[index];

		if ((entry & X86_PTE_PRESENT) == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!create) return false;
			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			table[index] = (uint64_t)next_phys | X86_PTE_PRESENT | X86_PTE_WRITE;
			if (user) table[index] |= X86_PTE_USER;
			entry = table[index];
		}
		else if ((entry & X86_PTE_LARGE) != 0) {
			return false;
		}
		else if (user && (entry & X86_PTE_USER) == 0) {
			table[index] = entry | X86_PTE_USER;
			entry        = table[index];
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & X86_PHYS_MASK));
	}

	*out_table = table;
	*out_index = (size_t)((virt >> 12) & 0x1ffu);
	return true;
}

bool hal_paging_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);

	kernel_space = (struct hal_address_space){
		.lower_root_phys = (uintptr_t)(x86_read_cr3() & X86_PHYS_MASK),
		.upper_root_phys = 0u,
		.flags           = (uint64_t)x86_paging_levels(),
	};
	initialized = x86_space_root_table(&kernel_space) != NULL;
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
	int              levels;
	struct irq_state state;

	if (out_space == NULL || !initialized) return false;

	state = spinlock_lock_irqsave(&paging_lock);
	if (!pmm_alloc_pages(1, &root_phys)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	root        = (uint64_t*)hhdm_phys_to_virt(root_phys);
	kernel_root = x86_space_root_table(&kernel_space);
	if (kernel_root == NULL) {
		(void)pmm_free_pages(root_phys, 1);
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	memset(root, 0, PMM_PAGE_SIZE);
	for (size_t index = 256u; index < 512u; index++) {
		root[index] = kernel_root[index];
	}
	levels     = x86_paging_levels();
	*out_space = (struct hal_address_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = 0u,
		.flags           = (uint64_t)levels,
	};
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static void x86_free_page_table_children(uint64_t* table, int level, size_t entry_count) {
	if (table == NULL || level <= 0) return;
	for (size_t index = 0u; index < entry_count; index++) {
		uint64_t entry = table[index];
		if ((entry & X86_PTE_PRESENT) == 0u || (entry & X86_PTE_LARGE) != 0u) continue;

		uintptr_t child_phys = (uintptr_t)(entry & X86_PHYS_MASK);
		uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
		x86_free_page_table_children(child, level - 1, 512u);
		(void)pmm_free_pages(child_phys, 1u);
	}
}

void hal_paging_space_destroy(struct hal_address_space* space) {
	struct irq_state state;
	uintptr_t        root_phys;
	uint64_t*        root;
	int              levels;

	if (space == NULL || space->lower_root_phys == 0u) return;
	root_phys = space->lower_root_phys & X86_PHYS_MASK;
	if (root_phys == kernel_space.lower_root_phys) return;
	state  = spinlock_lock_irqsave(&paging_lock);
	root   = (uint64_t*)hhdm_phys_to_virt(root_phys);
	levels = space->flags != 0u ? (int)space->flags : x86_paging_levels();
	x86_free_page_table_children(root, levels - 1, 256u);
	(void)pmm_free_pages(root_phys, 1u);
	*space = (struct hal_address_space){0};
	spinlock_unlock_irqrestore(&paging_lock, state);
}

bool hal_paging_activate(const struct hal_address_space* space) {
	struct irq_state state;

	if (space == NULL || space->lower_root_phys == 0u || !initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	x86_write_cr3(space->lower_root_phys & X86_PHYS_MASK);
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
	if (!x86_walk_to_leaf(space, virt, true, (flags & HAL_PAGE_USER) != 0, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & X86_PTE_PRESENT) != 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = (uint64_t)phys | x86_leaf_flags(flags);
	x86_invlpg(virt);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool x86_table_empty(const uint64_t* table) {
	for (size_t i = 0u; i < 512u; i++)
		if ((table[i] & X86_PTE_PRESENT) != 0u) return false;
	return true;
}

static bool x86_change_range(uint64_t* table, int level, uintptr_t start, uintptr_t end, bool protect, uint64_t flags) {
	unsigned  shift = 12u + 9u * (unsigned)level;
	uintptr_t span  = (uintptr_t)1u << shift;
	while (start < end) {
		size_t    index = (size_t)((start >> shift) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & X86_PTE_PRESENT) != 0u) {
			if (level == 0) {
				if (protect) table[index] = (entry & X86_PHYS_MASK) | x86_leaf_flags(flags);
				else table[index] = 0u;
				x86_invlpg(start);
			}
			else {
				if ((entry & X86_PTE_LARGE) != 0u) return false;
				uintptr_t child_phys = (uintptr_t)(entry & X86_PHYS_MASK);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!x86_change_range(child, level - 1, start, next, protect, flags)) return false;
				if (!protect && x86_table_empty(child) && pmm_free_pages(child_phys, 1u)) table[index] = 0u;
			}
		}
		start = next;
	}
	return true;
}

static bool x86_range_args(struct hal_address_space* space, uintptr_t virt, size_t page_count, uintptr_t* out_end) {
	uint64_t bytes, end;
	if (space == NULL || !initialized || page_count == 0u || (virt & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    mul_overflow_u64(page_count, PMM_PAGE_SIZE, &bytes) || add_overflow_u64(virt, bytes, &end))
		return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap_range(struct hal_address_space* space, uintptr_t virt, size_t page_count) {
	uintptr_t end;
	if (!x86_range_args(space, virt, page_count, &end)) return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = x86_space_root_table(space);
	bool             ok    = root != NULL && x86_change_range(root, x86_paging_levels() - 1, virt, end, false, 0u);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_protect_range(struct hal_address_space* space, uintptr_t virt, size_t page_count, uint64_t flags) {
	uintptr_t end;
	if ((flags & ~HAL_PAGE_VALID_MASK) != 0u || !x86_range_args(space, virt, page_count, &end)) return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = x86_space_root_table(space);
	bool             ok    = root != NULL && x86_change_range(root, x86_paging_levels() - 1, virt, end, true, flags);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return ok;
}

bool hal_paging_query(const struct hal_address_space* space, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	uint64_t         entry;
	uint64_t         page_offset = virt & (PMM_PAGE_SIZE - 1u);
	uint64_t         flags       = 0;
	struct irq_state state;

	if (out_phys) *out_phys = 0;
	if (out_flags) *out_flags = 0;

	if (space == NULL) return false;
	if (!initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	if (!x86_walk_to_leaf(space, virt, false, false, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	entry = table[index];
	if ((entry & X86_PTE_PRESENT) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	if ((entry & X86_PTE_WRITE) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & X86_PTE_NX) == 0) flags |= HAL_PAGE_EXEC;
	if ((entry & X86_PTE_GLOBAL) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & (X86_PTE_PCD | X86_PTE_PWT)) != 0) flags |= HAL_PAGE_NO_CACHE;
	if ((entry & X86_PTE_USER) != 0) flags |= HAL_PAGE_USER;

	if (out_phys) {
		uint64_t phys;
		if (add_overflow_u64(entry & X86_PHYS_MASK, page_offset, &phys)) {
			spinlock_unlock_irqrestore(&paging_lock, state);
			return false;
		}
		*out_phys = (uintptr_t)phys;
	}
	if (out_flags) *out_flags = flags;

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}
