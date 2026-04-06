#include <core/math.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define X86_PTE_PRESENT (1ull << 0)
#define X86_PTE_WRITE (1ull << 1)
#define X86_PTE_PWT (1ull << 3)
#define X86_PTE_PCD (1ull << 4)
#define X86_PTE_LARGE (1ull << 7)
#define X86_PTE_GLOBAL (1ull << 8)
#define X86_PTE_NX (1ull << 63)
#define X86_CR4_LA57 (1ull << 12)
#define X86_PHYS_MASK 0x000ffffffffff000ull

static bool initialized;

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

	return entry;
}

static inline uint64_t* x86_root_table(void) {
	uintptr_t root_phys = (uintptr_t)(x86_read_cr3() & X86_PHYS_MASK);
	if (root_phys == 0) return NULL;

	return (uint64_t*)hhdm_phys_to_virt(root_phys);
}

static inline int x86_paging_levels(void) {
	return (x86_read_cr4() & X86_CR4_LA57) != 0 ? 5 : 4;
}

static bool x86_walk_to_leaf(uintptr_t virt, bool create, uint64_t** out_table, size_t* out_index) {
	uint64_t* table  = x86_root_table();
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
			entry        = table[index];
		}
		else if ((entry & X86_PTE_LARGE) != 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & X86_PHYS_MASK));
	}

	*out_table = table;
	*out_index = (size_t)((virt >> 12) & 0x1ffu);
	return true;
}

bool hal_paging_init(void) {
	initialized = x86_root_table() != NULL;
	return initialized;
}

bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
	uint64_t* table = NULL;
	size_t    index = 0;

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (!x86_walk_to_leaf(virt, true, &table, &index)) return false;
	if ((table[index] & X86_PTE_PRESENT) != 0) return false;

	table[index] = (uint64_t)phys | x86_leaf_flags(flags);
	x86_invlpg(virt);
	return true;
}

bool hal_paging_unmap(uintptr_t virt) {
	uint64_t* table = NULL;
	size_t    index = 0;

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (!x86_walk_to_leaf(virt, false, &table, &index)) return false;
	if ((table[index] & X86_PTE_PRESENT) == 0) return false;

	table[index] = 0;
	x86_invlpg(virt);
	return true;
}

bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	uint64_t* table = NULL;
	size_t    index = 0;
	uint64_t  entry;
	uint64_t  page_offset = virt & (PMM_PAGE_SIZE - 1u);
	uint64_t  flags       = 0;

	if (out_phys) *out_phys = 0;
	if (out_flags) *out_flags = 0;

	if (!initialized) return false;
	if (!x86_walk_to_leaf(virt, false, &table, &index)) return false;

	entry = table[index];
	if ((entry & X86_PTE_PRESENT) == 0) return false;

	if ((entry & X86_PTE_WRITE) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & X86_PTE_NX) == 0) flags |= HAL_PAGE_EXEC;
	if ((entry & X86_PTE_GLOBAL) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & (X86_PTE_PCD | X86_PTE_PWT)) != 0) flags |= HAL_PAGE_NO_CACHE;

	if (out_phys) {
		uint64_t phys;
		if (add_overflow_u64(entry & X86_PHYS_MASK, page_offset, &phys)) return false;
		*out_phys = (uintptr_t)phys;
	}
	if (out_flags) *out_flags = flags;

	return true;
}
