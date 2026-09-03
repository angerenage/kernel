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

#include "interrupts_private.h"

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
#define X86_TLB_MAX_CPUS 64u

struct hal_paging_space {
	uintptr_t lower_root_phys;
	uintptr_t upper_root_phys;
	uint64_t  flags;
	uintptr_t storage_phys;
};

static const struct hal_paging_info paging_info = {
	.minimum_leaf_size = PMM_PAGE_SIZE,
	.leaf_size_mask    = 1ull << 12,
};

struct x86_tlb_request {
	uintptr_t start;
	size_t    page_count;
	size_t    source_index;
	uint64_t  generation;
};

static bool                    initialized;
static struct hal_paging_space kernel_space;
static struct spinlock         paging_lock =
	SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
static struct x86_tlb_request x86_tlb_request;
static uint64_t               x86_tlb_ack[X86_TLB_MAX_CPUS];
static struct spinlock        x86_tlb_shootdown_lock = SPINLOCK_INIT_CLASS(
    "tlb_shootdown_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

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

static void x86_tlb_flush_range_local(uintptr_t start, size_t page_count) {
	for (size_t i = 0u; i < page_count; i++) {
		x86_invlpg(start + i * (uintptr_t)PMM_PAGE_SIZE);
	}
}

static bool x86_tlb_process_request_for_current_cpu(void) {
	struct cpu* cpu = cpu_current();
	uint64_t    generation;
	uint64_t    acknowledged;

	if (cpu == NULL || cpu->index >= X86_TLB_MAX_CPUS) hcf();
	generation = __atomic_load_n(&x86_tlb_request.generation, __ATOMIC_ACQUIRE);
	if (generation == 0u || cpu->index == x86_tlb_request.source_index) return false;
	acknowledged = __atomic_load_n(&x86_tlb_ack[cpu->index], __ATOMIC_ACQUIRE);
	if (acknowledged == generation) return false;

	x86_tlb_flush_range_local(x86_tlb_request.start, x86_tlb_request.page_count);
	__atomic_store_n(&x86_tlb_ack[cpu->index], generation, __ATOMIC_RELEASE);
	return true;
}

bool x86_64_paging_handle_tlb_nmi(void) {
	return x86_tlb_process_request_for_current_cpu();
}

static void x86_tlb_shootdown_range(uintptr_t start, size_t page_count) {
	uint64_t                   targets = 0u;
	struct cpu*                current = cpu_current();
	const struct cpu_topology* topology;
	size_t                     cpu_total;
	uint64_t                   generation;
	struct irq_state           shootdown_state;

	if (page_count == 0u) return;
	shootdown_state = spinlock_lock_irqsave(&x86_tlb_shootdown_lock);
	x86_tlb_flush_range_local(start, page_count);

	topology = cpu_topology_get();
	if (current == NULL || topology == NULL || topology->cpus == NULL || current->index >= X86_TLB_MAX_CPUS ||
	    topology->cpu_count == 0u || topology->cpu_count > X86_TLB_MAX_CPUS) {
		hcf();
	}
	cpu_total = topology->cpu_count;

	bool has_remote_target = false;
	for (size_t i = 0u; i < cpu_total; i++) {
		struct cpu* target = &topology->cpus[i];
		if (target != current && cpu_state_get(target) == CPU_STATE_ONLINE) {
			has_remote_target = true;
			break;
		}
	}
	if (!has_remote_target) {
		spinlock_unlock_irqrestore(&x86_tlb_shootdown_lock, shootdown_state);
		return;
	}
	if (!apic_ipi_ready()) hcf();

	generation = __atomic_load_n(&x86_tlb_request.generation, __ATOMIC_RELAXED) + 1u;
	if (generation == 0u) generation = 1u;

	x86_tlb_request.start        = start;
	x86_tlb_request.page_count   = page_count;
	x86_tlb_request.source_index = current->index;
	__atomic_store_n(&x86_tlb_request.generation, generation, __ATOMIC_RELEASE);
	/* Publish the request before the LAPIC MMIO write that delivers the NMI. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (size_t i = 0u; i < cpu_total; i++) {
		struct cpu* target = &topology->cpus[i];

		if (target == current || target->index >= X86_TLB_MAX_CPUS || cpu_state_get(target) != CPU_STATE_ONLINE) {
			continue;
		}
		targets |= 1ull << target->index;
		if (!apic_send_nmi((uint32_t)target->arch_id)) hcf();
	}

	for (size_t target_index = 0u; target_index < cpu_total; target_index++) {
		if ((targets & (1ull << target_index)) == 0u) continue;
		while (__atomic_load_n(&x86_tlb_ack[target_index], __ATOMIC_ACQUIRE) != generation) {
			__asm__ volatile("pause" : : : "memory");
		}
	}
	spinlock_unlock_irqrestore(&x86_tlb_shootdown_lock, shootdown_state);
}

static inline uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + boot_info.direct_map_offset;
}

static inline uint64_t x86_leaf_flags(uint64_t flags, enum memory_type memory_type) {
	uint64_t entry = X86_PTE_PRESENT;

	if ((flags & HAL_PAGE_WRITE) != 0) entry |= X86_PTE_WRITE;
	if ((flags & HAL_PAGE_EXEC) == 0) entry |= X86_PTE_NX;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= X86_PTE_GLOBAL;
	if ((flags & HAL_PAGE_USER) != 0) entry |= X86_PTE_USER;
	if (memory_type == MEMORY_TYPE_DEVICE) entry |= X86_PTE_PWT | X86_PTE_PCD;

	return entry;
}

static inline enum memory_type x86_leaf_memory_type(uint64_t entry) {
	return (entry & (X86_PTE_PWT | X86_PTE_PCD)) != 0u ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL;
}

static inline uint64_t* x86_space_root_table(const struct hal_paging_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(space->lower_root_phys & X86_PHYS_MASK);
}

static inline int x86_paging_levels(void) {
	return (x86_read_cr4() & X86_CR4_LA57) != 0 ? 5 : 4;
}

static bool x86_walk_to_leaf(const struct hal_paging_space* space, uintptr_t virt, bool create, bool user,
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

	kernel_space = (struct hal_paging_space){
		.lower_root_phys = (uintptr_t)(x86_read_cr3() & X86_PHYS_MASK),
		.upper_root_phys = 0u,
		.flags           = (uint64_t)x86_paging_levels(),
	};
	initialized = x86_space_root_table(&kernel_space) != NULL;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return initialized;
}

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

static bool x86_query_entry(const struct hal_paging_space* space, uintptr_t virt, uint64_t* out_entry,
                            unsigned* out_shift) {
	uint64_t* table = x86_space_root_table(space);
	if (table == NULL || out_entry == NULL || out_shift == NULL) return false;
	for (int level = x86_paging_levels() - 1; level >= 0; level--) {
		uint64_t entry = table[(virt >> (12u + 9u * (unsigned)level)) & 0x1ffu];
		if ((entry & X86_PTE_PRESENT) == 0u) return false;
		if (level == 0 || (entry & X86_PTE_LARGE) != 0u) {
			*out_entry = entry;
			*out_shift = 12u + 9u * (unsigned)level;
			return true;
		}
		table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & X86_PHYS_MASK));
	}
	return false;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && (flags & HAL_PAGE_READ) != 0u && memory_type < MEMORY_TYPE_COUNT;
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
	int                      levels;
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
	kernel_root = x86_space_root_table(&kernel_space);
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
	levels = x86_paging_levels();
	*space = (struct hal_paging_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = 0u,
		.flags           = (uint64_t)levels,
		.storage_phys    = storage_phys,
	};
	*out_space = space;
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

void hal_paging_space_destroy(struct hal_paging_space* space) {
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
	uintptr_t storage_phys = space->storage_phys;
	*space                 = (struct hal_paging_space){0};
	if (storage_phys != 0u) (void)pmm_free_pages(storage_phys, 1u);
	spinlock_unlock_irqrestore(&paging_lock, state);
}

bool hal_paging_activate(const struct hal_paging_space* space) {
	struct irq_state state;

	if (space == NULL || space->lower_root_phys == 0u || !initialized) return false;
	state = spinlock_lock_irqsave(&paging_lock);
	x86_write_cr3(space->lower_root_phys & X86_PHYS_MASK);
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool x86_map_page(struct hal_paging_space* space, uintptr_t virt, uintptr_t phys, uint64_t flags,
                         enum memory_type memory_type) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	struct irq_state state;

	if (space == NULL) return false;
	if (!initialized) return false;
	if ((flags & ~HAL_PAGE_VALID_MASK) != 0 || memory_type >= MEMORY_TYPE_COUNT) return false;
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

	table[index] = (uint64_t)phys | x86_leaf_flags(flags, memory_type);
	spinlock_unlock_irqrestore(&paging_lock, state);
	x86_tlb_shootdown_range(virt, 1u);
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
				if (protect)
					table[index] = (entry & X86_PHYS_MASK) | x86_leaf_flags(flags, x86_leaf_memory_type(entry));
				else table[index] = 0u;
			}
			else {
				if ((entry & X86_PTE_LARGE) != 0u) return false;
				uintptr_t child_phys = (uintptr_t)(entry & X86_PHYS_MASK);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!x86_change_range(child, level - 1, start, next, protect, flags)) return false;
			}
		}
		start = next;
	}
	return true;
}

static bool x86_can_change_range(uint64_t* table, int level, uintptr_t start, uintptr_t end) {
	unsigned  shift = 12u + 9u * (unsigned)level;
	uintptr_t span  = (uintptr_t)1u << shift;
	while (start < end) {
		size_t    index = (size_t)((start >> shift) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & X86_PTE_PRESENT) != 0u && level > 0) {
			if ((entry & X86_PTE_LARGE) != 0u ||
			    !x86_can_change_range(
					(uint64_t*)hhdm_phys_to_virt((uintptr_t)(entry & X86_PHYS_MASK)), level - 1, start, next))
				return false;
		}
		start = next;
	}
	return true;
}

static bool x86_range_args(struct hal_paging_space* space, uintptr_t virt, size_t size, uintptr_t* out_end) {
	uint64_t end;
	if (space == NULL || !initialized || size == 0u || (virt & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    (size & (PMM_PAGE_SIZE - 1u)) != 0u || add_overflow_u64(virt, size, &end))
		return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	uintptr_t end;
	if (!x86_range_args(space, virt, size, &end)) return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = x86_space_root_table(space);
	bool             ok    = root != NULL && x86_can_change_range(root, x86_paging_levels() - 1, virt, end) &&
	          x86_change_range(root, x86_paging_levels() - 1, virt, end, false, 0u);
	spinlock_unlock_irqrestore(&paging_lock, state);
	if (ok) x86_tlb_shootdown_range(virt, size / PMM_PAGE_SIZE);
	return ok;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	uintptr_t end;
	if (!hal_paging_mapping_supported(flags, MEMORY_TYPE_NORMAL) || !x86_range_args(space, virt, size, &end))
		return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = x86_space_root_table(space);
	bool             ok    = root != NULL && x86_can_change_range(root, x86_paging_levels() - 1, virt, end) &&
	          x86_change_range(root, x86_paging_levels() - 1, virt, end, true, flags);
	spinlock_unlock_irqrestore(&paging_lock, state);
	if (ok) x86_tlb_shootdown_range(virt, size / PMM_PAGE_SIZE);
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
	if (!x86_query_entry(space, virt, &entry, &shift)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	if ((entry & X86_PTE_WRITE) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & X86_PTE_NX) == 0) flags |= HAL_PAGE_EXEC;
	if ((entry & X86_PTE_GLOBAL) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((entry & X86_PTE_USER) != 0) flags |= HAL_PAGE_USER;

	uintptr_t physical_address;
	{
		uint64_t phys;
		uint64_t leaf_mask = ((uint64_t)1u << shift) - 1u;
		if (add_overflow_u64(entry & X86_PHYS_MASK & ~leaf_mask, virt & leaf_mask, &phys)) {
			spinlock_unlock_irqrestore(&paging_lock, state);
			return false;
		}
		physical_address = (uintptr_t)phys;
	}
	if (out_translation)
		*out_translation = (struct hal_paging_translation){
			.physical_address = physical_address,
			.leaf_size        = (size_t)1u << shift,
			.flags            = flags | HAL_PAGE_READ,
			.memory_type      = x86_leaf_memory_type(entry),
		};

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	if (request == NULL || !hal_paging_mapping_supported(request->flags, request->memory_type) || request->size == 0u ||
	    (request->virtual_address & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    (request->physical_address & (PMM_PAGE_SIZE - 1u)) != 0u || (request->size & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address)
		return false;
	size_t mapped = 0u;
	while (mapped < request->size) {
		if (!x86_map_page(space,
		                  request->virtual_address + mapped,
		                  request->physical_address + mapped,
		                  request->flags,
		                  request->memory_type)) {
			if (mapped != 0u) (void)hal_paging_unmap(space, request->virtual_address, mapped);
			return false;
		}
		mapped += PMM_PAGE_SIZE;
	}
	return true;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}
