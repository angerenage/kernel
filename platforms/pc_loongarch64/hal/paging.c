#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
#define LOONGARCH_PTE_P (1ull << 7)
#define LOONGARCH_PTE_W (1ull << 8)
#define LOONGARCH_PTE_NR (1ull << 61)
#define LOONGARCH_PTE_NX (1ull << 62)
#define LOONGARCH_PTE_RPLV (1ull << 63)

static bool                     initialized;
static uint64_t                 phys_mask;
static unsigned                 palen_bits;
static unsigned                 valen_bits;
static struct hal_address_space kernel_space;
static struct spinlock          paging_lock =
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

static inline void loongarch_tlb_flush_all(void) {
	__asm__ volatile("invtlb 0x0, $r0, $r0" ::: "memory");
}

static inline void loongarch_page_table_sync(void) {
	__asm__ volatile("dbar 0\n\t"
	                 "ibar 0" ::
	                     : "memory");
}

static inline bool loongarch_upper_half(uintptr_t virt) {
	return ((uint64_t)virt >> (palen_bits - 1u)) != 0;
}

static inline uint64_t loongarch_root_phys(uintptr_t virt) {
	return loongarch_upper_half(virt) ? (loongarch_csrrd(LOONGARCH_CSR_PGDH) & phys_mask)
	                                  : (loongarch_csrrd(LOONGARCH_CSR_PGDL) & phys_mask);
}

static inline uint64_t loongarch_space_root_phys(const struct hal_address_space* space, uintptr_t virt) {
	if (space == NULL) return loongarch_root_phys(virt);
	return loongarch_upper_half(virt) ? (space->upper_root_phys & phys_mask) : (space->lower_root_phys & phys_mask);
}

static inline uint64_t loongarch_common_flags(uint64_t flags) {
	uint64_t entry = LOONGARCH_PTE_P | LOONGARCH_PTE_V;
	uint64_t mat   = ((flags & HAL_PAGE_NO_CACHE) != 0) ? 0ull : (uint64_t)LOONGARCH_CRMD_CC;

	entry |= ((flags & HAL_PAGE_USER) != 0) ? LOONGARCH_PTE_PLV3 : LOONGARCH_PTE_PLV0;
	entry |= mat << LOONGARCH_PTE_MAT_SHIFT;
	if ((flags & HAL_PAGE_WRITE) != 0) entry |= LOONGARCH_PTE_W | LOONGARCH_PTE_D;
	if ((flags & HAL_PAGE_EXEC) == 0) entry |= LOONGARCH_PTE_NX;
	if ((flags & HAL_PAGE_GLOBAL) != 0) entry |= LOONGARCH_PTE_G;

	return entry;
}

static bool loongarch_walk_to_leaf_in(const struct hal_address_space* space, uintptr_t virt, bool create,
                                      uint64_t** out_table, size_t* out_index) {
	static const unsigned shifts[]  = {39u, 30u, 21u, 12u};
	uint64_t              root_phys = loongarch_space_root_phys(space, virt);
	uint64_t*             table;

	if (!out_table || !out_index) return false;
	if (root_phys == 0) return false;

	table = (uint64_t*)hhdm_phys_to_virt((uintptr_t)root_phys);

	for (size_t level = 0; level < 3; level++) {
		size_t   index = (size_t)((virt >> shifts[level]) & 0x1ffu);
		uint64_t entry = table[index];

		if (entry == 0) {
			uintptr_t next_phys = 0;
			uint64_t* next_table;

			if (!create) return false;
			if (!pmm_alloc_pages(1, &next_phys)) return false;

			next_table = (uint64_t*)hhdm_phys_to_virt(next_phys);
			memset(next_table, 0, PMM_PAGE_SIZE);
			table[index] = loongarch_entry_from_phys(next_phys);
			entry        = table[index];
		}
		else if ((entry & LOONGARCH_PTE_G) != 0) {
			return false;
		}

		table = (uint64_t*)hhdm_phys_to_virt(loongarch_entry_to_phys(entry));
	}

	*out_table = table;
	*out_index = (size_t)((virt >> shifts[3]) & 0x1ffu);
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

	phys_mask = (((1ull << palen_bits) - 1u) & ~(uint64_t)(PMM_PAGE_SIZE - 1u));
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

	kernel_space = (struct hal_address_space){
		.lower_root_phys = (uintptr_t)(loongarch_csrrd(LOONGARCH_CSR_PGDL) & phys_mask),
		.upper_root_phys = (uintptr_t)(loongarch_csrrd(LOONGARCH_CSR_PGDH) & phys_mask),
		.flags           = 0u,
	};
	initialized = true;
	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

struct hal_address_space* hal_paging_kernel_space(void) {
	return initialized ? &kernel_space : NULL;
}

bool hal_paging_space_create(struct hal_address_space* out_space) {
	uintptr_t        root_phys = 0;
	uint64_t*        root;
	struct irq_state state;

	if (out_space == NULL || !initialized) return false;

	state = spinlock_lock_irqsave(&paging_lock);
	if (!pmm_alloc_pages(1, &root_phys)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	root = (uint64_t*)hhdm_phys_to_virt(root_phys);
	memset(root, 0, PMM_PAGE_SIZE);
	*out_space = (struct hal_address_space){
		.lower_root_phys = root_phys,
		.upper_root_phys = kernel_space.upper_root_phys,
		.flags           = 0u,
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
	if (!loongarch_walk_to_leaf_in(space, virt, true, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & LOONGARCH_PTE_P) != 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = loongarch_entry_from_phys(phys) | loongarch_common_flags(flags);
	loongarch_page_table_sync();
	loongarch_tlb_flush_all();
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
	if (!loongarch_walk_to_leaf_in(space, virt, false, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}
	if ((table[index] & LOONGARCH_PTE_P) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	table[index] = 0;
	loongarch_page_table_sync();
	loongarch_tlb_flush_all();
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
	if (!loongarch_walk_to_leaf_in(space, virt, false, &table, &index)) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	entry = table[index];
	if ((entry & LOONGARCH_PTE_P) == 0 || (entry & LOONGARCH_PTE_V) == 0) {
		spinlock_unlock_irqrestore(&paging_lock, state);
		return false;
	}

	if (out_phys) *out_phys = loongarch_entry_to_phys(entry) | (virt & (PMM_PAGE_SIZE - 1u));
	if ((entry & LOONGARCH_PTE_W) != 0) flags |= HAL_PAGE_WRITE;
	if ((entry & LOONGARCH_PTE_NX) == 0) flags |= HAL_PAGE_EXEC;
	if ((entry & LOONGARCH_PTE_G) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((((entry >> LOONGARCH_PTE_MAT_SHIFT) & 0x3u) != LOONGARCH_CRMD_CC)) flags |= HAL_PAGE_NO_CACHE;
	if ((entry & LOONGARCH_PTE_PLV_MASK) == LOONGARCH_PTE_PLV3) flags |= HAL_PAGE_USER;
	if (out_flags) *out_flags = flags;

	spinlock_unlock_irqrestore(&paging_lock, state);
	return true;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}
