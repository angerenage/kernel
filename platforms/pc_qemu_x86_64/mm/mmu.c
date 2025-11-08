#include <hal/mm.h>
#include <hal/mmu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PAGE_TABLE_ENTRIES 512u
#define X86_PTE_PRESENT    (1ull << 0)
#define X86_PTE_WRITABLE   (1ull << 1)
#define X86_PTE_USER       (1ull << 2)
#define X86_PTE_PWT        (1ull << 3)
#define X86_PTE_PCD        (1ull << 4)
#define X86_PTE_ACCESSED   (1ull << 5)
#define X86_PTE_DIRTY      (1ull << 6)
#define X86_PTE_HUGE       (1ull << 7)
#define X86_PTE_GLOBAL     (1ull << 8)
#define X86_PTE_NX         (1ull << 63)

#define MSR_EFER 0xc0000080u
#define EFER_NXE (1ull << 11)

struct page_table {
	uint64_t entries[PAGE_TABLE_ENTRIES];
};

static struct page_table* kernel_pml4_virt;
static uintptr_t          kernel_pml4_phys;
static bool               nx_enabled;

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
	__asm__ volatile("cpuid"
	                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
	                 : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdmsr(uint32_t msr) {
	uint32_t lo;
	uint32_t hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
	uint32_t lo = (uint32_t)value;
	uint32_t hi = (uint32_t)(value >> 32);
	__asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static void detect_and_enable_nx(void) {
	uint32_t max_ext = 0;
	uint32_t ebx     = 0;
	uint32_t ecx     = 0;
	uint32_t edx     = 0;

	cpuid(0x80000000u, 0, &max_ext, &ebx, &ecx, &edx);
	if (max_ext < 0x80000001u) {
		return;
	}

	uint32_t eax = 0;
	cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
	if (!(edx & (1u << 20))) {
		return;
	}

	uint64_t efer = rdmsr(MSR_EFER);
	if (!(efer & EFER_NXE)) {
		efer |= EFER_NXE;
		wrmsr(MSR_EFER, efer);
	}

	if (efer & EFER_NXE) {
		nx_enabled = true;
	}
}

static inline uintptr_t align_down(uintptr_t value, uintptr_t alignment) {
	return value & ~(alignment - 1u);
}

static inline bool is_aligned(uintptr_t value, uintptr_t alignment) {
	return (value & (alignment - 1u)) == 0;
}

static struct page_table* alloc_page_table(void) {
	void* virt = hal_mm_early_alloc(HAL_MMU_PAGE_SIZE, HAL_MMU_PAGE_SIZE);
	if (!virt) return NULL;

	memset(virt, 0, HAL_MMU_PAGE_SIZE);
	return (struct page_table*)virt;
}

static struct page_table* virt_from_entry(uint64_t entry) {
	uintptr_t phys = align_down(entry, HAL_MMU_PAGE_SIZE);
	return (struct page_table*)hal_mm_phys_to_virt(phys);
}

static uint64_t entry_flags_from_hal(uint32_t hal_flags, bool leaf) {
	uint64_t flags = X86_PTE_PRESENT;

	if (hal_flags & HAL_MMU_FLAG_WRITE) {
		flags |= X86_PTE_WRITABLE;
	}

	if (hal_flags & HAL_MMU_FLAG_USER) {
		flags |= X86_PTE_USER;
	}

	if (leaf && (hal_flags & HAL_MMU_FLAG_GLOBAL)) {
		flags |= X86_PTE_GLOBAL;
	}

	if (leaf && !(hal_flags & HAL_MMU_FLAG_EXECUTE) && nx_enabled) {
		flags |= X86_PTE_NX;
	}

	if (!leaf) {
		flags &= ~X86_PTE_NX;
	}

	return flags;
}

static struct page_table* ensure_table(struct page_table* table, size_t index, uint32_t hal_flags) {
	uint64_t entry = table->entries[index];

	if (!(entry & X86_PTE_PRESENT)) {
		struct page_table* new_table = alloc_page_table();
		if (!new_table) return NULL;

		uintptr_t phys = hal_mm_virt_to_phys((uintptr_t)new_table);
		if (!phys) return NULL;

		table->entries[index] = phys | entry_flags_from_hal(hal_flags | HAL_MMU_FLAG_WRITE | HAL_MMU_FLAG_EXECUTE, false);
		entry                 = table->entries[index];
	}

	return virt_from_entry(entry);
}

static bool walk_tables(uintptr_t virt_addr, bool create, uint32_t flags, struct page_table** out_pt, size_t* out_index) {
	size_t l4 = (virt_addr >> 39) & 0x1ffu;
	size_t l3 = (virt_addr >> 30) & 0x1ffu;
	size_t l2 = (virt_addr >> 21) & 0x1ffu;
	size_t l1 = (virt_addr >> 12) & 0x1ffu;

	struct page_table* table = kernel_pml4_virt;
	if (!table) return false;

	struct page_table* next;

	if (!(table->entries[l4] & X86_PTE_PRESENT)) {
		if (!create) return false;
		next = ensure_table(table, l4, flags);
	}
	else {
		next = virt_from_entry(table->entries[l4]);
	}
	if (!next) {
		return false;
	}
	table = next;

	if (!(table->entries[l3] & X86_PTE_PRESENT)) {
		if (!create) return false;
		next = ensure_table(table, l3, flags);
	}
	else {
		next = virt_from_entry(table->entries[l3]);
	}
	if (!next) {
		return false;
	}
	table = next;

	if (!(table->entries[l2] & X86_PTE_PRESENT)) {
		if (!create) return false;
		next = ensure_table(table, l2, flags);
	}
	else {
		if (table->entries[l2] & X86_PTE_HUGE) return false;
		next = virt_from_entry(table->entries[l2]);
	}
	if (!next) {
		return false;
	}
	table = next;

	*out_pt = table;
	*out_index = l1;
	return true;
}

bool hal_mmu_init(void) {
	uintptr_t cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

	detect_and_enable_nx();

	kernel_pml4_phys = align_down(cr3, HAL_MMU_PAGE_SIZE);
	uintptr_t kernel_pml4_addr = hal_mm_phys_to_virt(kernel_pml4_phys);
	if (!kernel_pml4_addr) return false;

	kernel_pml4_virt = (struct page_table*)kernel_pml4_addr;
	return true;
}

bool hal_mmu_map(uintptr_t virt_addr, uintptr_t phys_addr, size_t size, uint32_t flags) {
	if (!is_aligned(virt_addr, HAL_MMU_PAGE_SIZE) || !is_aligned(phys_addr, HAL_MMU_PAGE_SIZE)) {
		return false;
	}

	if (size == 0 || (size % HAL_MMU_PAGE_SIZE) != 0) {
		return false;
	}

	for (size_t offset = 0; offset < size; offset += HAL_MMU_PAGE_SIZE) {
		struct page_table* pt;
		size_t             index;
		if (!walk_tables(virt_addr + offset, true, flags, &pt, &index)) {
			return false;
		}

		uintptr_t phys = phys_addr + offset;
		uint64_t  entry_flags = entry_flags_from_hal(flags, true);
		pt->entries[index]    = phys | entry_flags;
		hal_mmu_invalidate(virt_addr + offset);
	}

	return true;
}

bool hal_mmu_unmap(uintptr_t virt_addr, size_t size) {
	if (!is_aligned(virt_addr, HAL_MMU_PAGE_SIZE)) {
		return false;
	}

	if (size == 0 || (size % HAL_MMU_PAGE_SIZE) != 0) {
		return false;
	}

	for (size_t offset = 0; offset < size; offset += HAL_MMU_PAGE_SIZE) {
		struct page_table* pt;
		size_t             index;
		if (!walk_tables(virt_addr + offset, false, 0, &pt, &index)) {
			return false;
		}

		if (!(pt->entries[index] & X86_PTE_PRESENT)) {
			return false;
		}

		pt->entries[index] = 0;
		hal_mmu_invalidate(virt_addr + offset);
	}

	return true;
}

void hal_mmu_invalidate(uintptr_t virt_addr) {
	__asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}
