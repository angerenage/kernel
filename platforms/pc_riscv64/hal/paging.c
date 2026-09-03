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

#define RISCV_PTE_V (1ull << 0)
#define RISCV_PTE_R (1ull << 1)
#define RISCV_PTE_W (1ull << 2)
#define RISCV_PTE_X (1ull << 3)
#define RISCV_PTE_U (1ull << 4)
#define RISCV_PTE_G (1ull << 5)
#define RISCV_PTE_A (1ull << 6)
#define RISCV_PTE_D (1ull << 7)
#define RISCV_PTE_DEVICE (1ull << 8)

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

static const struct hal_paging_info paging_info = {
	.minimum_leaf_size = PMM_PAGE_SIZE,
	.leaf_size_mask    = 1ull << 12,
};

struct riscv_sbi_ret {
	long error;
	long value;
};

static bool                    initialized;
static int                     paging_levels;
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
	return (uintptr_t)((pte >> 10) << 12);
}

static inline uint64_t riscv_pte_from_phys(uintptr_t phys) {
	return ((uint64_t)phys >> 12) << 10;
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

static void riscv_tlb_shootdown_range(uintptr_t start, size_t page_count) {
	const struct cpu_topology* topology = cpu_topology_get();
	struct cpu*                current  = cpu_current();
	uint64_t                   byte_count;

	if (page_count == 0u) return;
	for (size_t i = 0u; i < page_count; i++) {
		riscv_tlb_flush(start + i * (uintptr_t)PMM_PAGE_SIZE);
	}

	if (topology == NULL || topology->cpus == NULL || current == NULL || topology->cpu_count == 0u ||
	    mul_overflow_u64(page_count, PMM_PAGE_SIZE, &byte_count)) {
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
		                      (unsigned long)start,
		                      (unsigned long)byte_count,
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
	if (memory_type == MEMORY_TYPE_DEVICE) entry |= RISCV_PTE_DEVICE;

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

static inline uint64_t* riscv_space_root_table(const struct hal_paging_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return NULL;
	return (uint64_t*)hhdm_phys_to_virt(space->lower_root_phys);
}

static bool riscv_walk_to_leaf_in(const struct hal_paging_space* space, uintptr_t virt, bool create,
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
	if (table == NULL || out_entry == NULL || out_shift == NULL) return false;
	for (int level = paging_levels - 1; level >= 0; level--) {
		uint64_t entry = table[(virt >> (12u + 9u * (unsigned)level)) & 0x1ffu];
		if ((entry & RISCV_PTE_V) == 0u) return false;
		if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u) {
			*out_entry = entry;
			*out_shift = 12u + 9u * (unsigned)level;
			return true;
		}
		if (level == 0) return false;
		table = (uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry));
	}
	return false;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && memory_type < MEMORY_TYPE_COUNT &&
	       (flags & (HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_EXEC)) != 0u &&
	       ((flags & HAL_PAGE_WRITE) == 0u || (flags & HAL_PAGE_READ) != 0u);
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
	riscv_write_satp(satp_prefix | ((uint64_t)space->lower_root_phys >> 12));
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

static bool riscv_map_page(struct hal_paging_space* space, uintptr_t virt, uintptr_t phys, uint64_t flags,
                           enum memory_type memory_type) {
	uint64_t*        table = NULL;
	size_t           index = 0;
	struct irq_state state;

	if (space == NULL) return false;
	if (!initialized) return false;
	if ((flags & ~HAL_PAGE_VALID_MASK) != 0 || memory_type >= MEMORY_TYPE_COUNT) return false;
	/*
	 * Base RISC-V page tables do not encode CPU memory type. Device-vs-normal
	 * semantics come from the platform's PMA/PBMT configuration;
	 */
	(void)memory_type;
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

	table[index] = riscv_pte_from_phys(phys) | riscv_leaf_flags(flags, memory_type);
	spinlock_unlock_irqrestore(&paging_lock, state);
	riscv_tlb_shootdown_range(virt, 1u);
	return true;
}

static bool riscv_change_range(uint64_t* table, int level, uintptr_t start, uintptr_t end, bool protect,
                               uint64_t flags) {
	unsigned  shift = 12u + 9u * (unsigned)level;
	uintptr_t span  = (uintptr_t)1u << shift;
	while (start < end) {
		size_t    index = (size_t)((start >> shift) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & RISCV_PTE_V) != 0u) {
			bool leaf = (entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u;
			if (level == 0) {
				if (!leaf) return false;
				if (protect)
					table[index] =
						riscv_pte_from_phys(riscv_pte_to_phys(entry)) |
						riscv_leaf_flags(flags,
					                     (entry & RISCV_PTE_DEVICE) != 0u ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL);
				else table[index] = 0u;
			}
			else {
				if (leaf) return false;
				uintptr_t child_phys = riscv_pte_to_phys(entry);
				uint64_t* child      = (uint64_t*)hhdm_phys_to_virt(child_phys);
				if (!riscv_change_range(child, level - 1, start, next, protect, flags)) return false;
			}
		}
		start = next;
	}
	return true;
}

static bool riscv_can_change_range(uint64_t* table, int level, uintptr_t start, uintptr_t end) {
	unsigned  shift = 12u + 9u * (unsigned)level;
	uintptr_t span  = (uintptr_t)1u << shift;
	while (start < end) {
		size_t    index = (size_t)((start >> shift) & 0x1ffu);
		uintptr_t step  = span - (start & (span - 1u));
		uintptr_t next  = step > end - start ? end : start + step;
		uint64_t  entry = table[index];
		if ((entry & RISCV_PTE_V) != 0u && level > 0) {
			if ((entry & (RISCV_PTE_R | RISCV_PTE_W | RISCV_PTE_X)) != 0u ||
			    !riscv_can_change_range((uint64_t*)hhdm_phys_to_virt(riscv_pte_to_phys(entry)), level - 1, start, next))
				return false;
		}
		start = next;
	}
	return true;
}

static bool riscv_range_args(struct hal_paging_space* space, uintptr_t virt, size_t size, uintptr_t* out_end) {
	uint64_t end;
	if (space == NULL || !initialized || size == 0u || (virt & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    (size & (PMM_PAGE_SIZE - 1u)) != 0u || add_overflow_u64(virt, size, &end))
		return false;
	*out_end = (uintptr_t)end;
	return true;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	uintptr_t end;
	if (!riscv_range_args(space, virt, size, &end)) return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = riscv_space_root_table(space);
	bool             ok    = root != NULL && riscv_can_change_range(root, paging_levels - 1, virt, end) &&
	          riscv_change_range(root, paging_levels - 1, virt, end, false, 0u);
	spinlock_unlock_irqrestore(&paging_lock, state);
	if (ok) riscv_tlb_shootdown_range(virt, size / PMM_PAGE_SIZE);
	return ok;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	uintptr_t end;
	if (!hal_paging_mapping_supported(flags, MEMORY_TYPE_NORMAL) || !riscv_range_args(space, virt, size, &end))
		return false;
	struct irq_state state = spinlock_lock_irqsave(&paging_lock);
	uint64_t*        root  = riscv_space_root_table(space);
	bool             ok    = root != NULL && riscv_can_change_range(root, paging_levels - 1, virt, end) &&
	          riscv_change_range(root, paging_levels - 1, virt, end, true, flags);
	spinlock_unlock_irqrestore(&paging_lock, state);
	if (ok) riscv_tlb_shootdown_range(virt, size / PMM_PAGE_SIZE);
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
			.memory_type      = (entry & RISCV_PTE_DEVICE) != 0u ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL,
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
		if (!riscv_map_page(space,
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
	__asm__ volatile("fence.i" : : : "memory");
}
