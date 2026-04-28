#include <base/math.h>
#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VMM_WINDOW_BASE MM_KERNEL_VMM_BASE
#define VMM_WINDOW_SIZE MM_KERNEL_VMM_SIZE
#define VMM_INITIAL_ALLOCATION_CAPACITY 16u
#define VMM_PAGE_ENTRY_MAPPED (uintptr_t)1u
#define VMM_PAGE_ENTRY_ROLLBACK_KEEP (uintptr_t)2u
#define VMM_PAGE_ENTRY_ROLLBACK_SKIP (uintptr_t)4u

struct vmm_alloc_record {
	vmm_id_t      id;
	uintptr_t     reserved_base;
	uintptr_t     base;
	size_t        reserved_page_count;
	size_t        page_count;
	vmm_prot_t    prot;
	enum vmm_kind kind;
	size_t        guard_pages;
	uint64_t      map_flags;
	size_t        mapped_page_count;
	uintptr_t*    phys_pages;
	uintptr_t     phys_array_phys;
	size_t        phys_array_page_count;
	bool          used;
};

static bool            initialized;
static struct spinlock vmm_lock =
	SPINLOCK_INIT_CLASS("vmm_lock", SPINLOCK_ORDER_VMM, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static inline uint64_t vmm_prot_to_hal_flags(vmm_prot_t prot) {
	uint64_t flags = 0;

	if ((prot & VMM_PROT_WRITE) != 0) flags |= HAL_PAGE_WRITE;
	if ((prot & VMM_PROT_EXEC) != 0) flags |= HAL_PAGE_EXEC;
	if ((prot & VMM_PROT_GLOBAL) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((prot & VMM_PROT_NO_CACHE) != 0) flags |= HAL_PAGE_NO_CACHE;
	if ((prot & VMM_PROT_USER) != 0) flags |= HAL_PAGE_USER;
	return flags;
}

static inline bool vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0;
}

static inline bool address_space_is_kernel(const struct address_space* space) {
	return space != NULL && space == address_space_kernel();
}

static inline struct hal_address_space* vmm_hal_space(struct address_space* space) {
	return address_space_hal(space);
}

static bool address_space_range_contains(const struct address_space* space, uintptr_t base, size_t page_count) {
	uint64_t span;
	uint64_t end;
	uint64_t space_span;
	uint64_t space_end;

	if (!space || page_count == 0 || !space->initialized) return false;
	if (mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span)) return false;
	if (add_overflow_u64((uint64_t)base, span, &end)) return false;
	if (mul_overflow_u64((uint64_t)space->total_pages, PMM_PAGE_SIZE, &space_span)) return false;
	if (add_overflow_u64((uint64_t)space->base, space_span, &space_end)) return false;

	return (uint64_t)base >= (uint64_t)space->base && end <= space_end;
}

static bool vmm_params_allowed_for_space(const struct address_space* space, const struct vmm_alloc_params* params) {
	if (!space || !params) return false;

	if (address_space_is_kernel(space)) {
		return (params->prot & VMM_PROT_USER) == 0;
	}

	if ((params->prot & VMM_PROT_USER) == 0) return false;
	if ((params->prot & VMM_PROT_GLOBAL) != 0) return false;

	switch (params->kind) {
	case VMM_KIND_GENERIC:
	case VMM_KIND_HEAP:
	case VMM_KIND_STACK:
		return true;
	case VMM_KIND_MMIO:
	case VMM_KIND_KERNEL_TEXT:
	case VMM_KIND_KERNEL_RODATA:
	case VMM_KIND_KERNEL_DATA:
	default:
		return false;
	}
}

static inline bool allocation_is_stack_locked(const struct vmm_alloc_record* allocation) {
	return allocation != NULL && allocation->kind == VMM_KIND_STACK;
}

static inline uintptr_t page_entry_phys(uintptr_t entry) {
	return entry & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static inline uintptr_t page_entry_flags(uintptr_t entry) {
	return entry & (uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static inline bool page_entry_has_backing(uintptr_t entry) {
	return page_entry_phys(entry) != 0;
}

static inline bool page_entry_is_mapped(uintptr_t entry) {
	return (page_entry_flags(entry) & VMM_PAGE_ENTRY_MAPPED) != 0;
}

static inline uintptr_t make_page_entry(uintptr_t phys, uintptr_t flags) {
	return page_entry_phys(phys) | flags;
}

static enum vmm_state allocation_state_locked(const struct vmm_alloc_record* allocation) {
	if (!allocation || allocation->mapped_page_count == 0) return VMM_STATE_RESERVED;
	if (allocation->mapped_page_count == allocation->page_count) return VMM_STATE_MAPPED;
	return VMM_STATE_PARTIAL;
}

static bool alloc_metadata_buffer(size_t bytes, void** out_virt, uintptr_t* out_phys, size_t* out_pages) {
	size_t    pages;
	uintptr_t phys = 0;

	if (out_virt) *out_virt = NULL;
	if (out_phys) *out_phys = 0;
	if (out_pages) *out_pages = 0;
	if (!out_virt || !out_phys || !out_pages || bytes == 0) return false;

	pages = (bytes + (size_t)PMM_PAGE_SIZE - 1u) / (size_t)PMM_PAGE_SIZE;
	if (pages == 0) return false;
	if (!pmm_alloc_pages(pages, &phys)) return false;

	*out_virt  = hhdm_phys_to_virt(phys);
	*out_phys  = phys;
	*out_pages = pages;
	memset(*out_virt, 0, pages * (size_t)PMM_PAGE_SIZE);
	return true;
}

static void free_metadata_buffer(uintptr_t phys, size_t pages) {
	if (pages == 0) return;
	(void)pmm_free_pages(phys, pages);
}

static struct vmm_alloc_record* find_allocation_by_id_locked(struct address_space* space, vmm_id_t id) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->allocations_capacity; i++) {
		if (!space->allocations[i].used) continue;
		if (space->allocations[i].id == id) return &space->allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_allocation_by_base_locked(struct address_space* space, uintptr_t base) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->allocations_capacity; i++) {
		if (!space->allocations[i].used) continue;
		if (space->allocations[i].base == base) return &space->allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_allocation_containing_locked(struct address_space* space, uintptr_t addr) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->allocations_capacity; i++) {
		uint64_t span;
		uint64_t end;

		if (!space->allocations[i].used) continue;
		if (mul_overflow_u64((uint64_t)space->allocations[i].page_count, PMM_PAGE_SIZE, &span)) continue;
		if (add_overflow_u64((uint64_t)space->allocations[i].base, span, &end)) continue;
		if ((uint64_t)addr < (uint64_t)space->allocations[i].base) continue;
		if ((uint64_t)addr >= end) continue;
		return &space->allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_free_allocation_slot_locked(struct address_space* space) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->allocations_capacity; i++) {
		if (!space->allocations[i].used) return &space->allocations[i];
	}

	return NULL;
}

static bool ensure_allocation_capacity_locked(struct address_space* space) {
	struct vmm_alloc_record* new_allocations;
	uintptr_t                new_allocations_phys;
	size_t                   new_allocations_page_count;
	size_t                   new_capacity;
	size_t                   bytes;

	if (!space) return false;
	if (find_free_allocation_slot_locked(space) != NULL) return true;

	new_capacity =
		space->allocations_capacity != 0 ? space->allocations_capacity * 2u : VMM_INITIAL_ALLOCATION_CAPACITY;
	if (new_capacity < space->allocations_capacity) return false;
	if (mul_overflow_size(new_capacity, sizeof(struct vmm_alloc_record), &bytes)) return false;
	if (!alloc_metadata_buffer(bytes, (void**)&new_allocations, &new_allocations_phys, &new_allocations_page_count)) {
		return false;
	}

	if (space->allocations != NULL && space->allocations_capacity != 0) {
		memcpy(new_allocations, space->allocations, space->allocations_capacity * sizeof(struct vmm_alloc_record));
		free_metadata_buffer(space->allocations_phys, space->allocations_page_count);
	}

	space->allocations            = new_allocations;
	space->allocations_phys       = new_allocations_phys;
	space->allocations_page_count = new_allocations_page_count;
	space->allocations_capacity   = new_capacity;
	if (space->next_allocation_id == 0u) space->next_allocation_id = 1u;
	return true;
}

static void release_allocation_backing_locked(struct vmm_alloc_record* allocation) {
	if (!allocation || !allocation->phys_pages) return;

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t phys = page_entry_phys(allocation->phys_pages[page]);

		if (phys != 0) (void)pmm_free_pages(phys, 1);
	}

	free_metadata_buffer(allocation->phys_array_phys, allocation->phys_array_page_count);
	allocation->phys_pages            = NULL;
	allocation->phys_array_phys       = 0;
	allocation->phys_array_page_count = 0;
	allocation->mapped_page_count     = 0;
}

static bool ensure_backing_array_locked(struct vmm_alloc_record* allocation) {
	size_t bytes;

	if (!allocation) return false;
	if (allocation->phys_pages != NULL) return true;
	if (mul_overflow_size(allocation->page_count, sizeof(uintptr_t), &bytes)) return false;

	return alloc_metadata_buffer(
		bytes, (void**)&allocation->phys_pages, &allocation->phys_array_phys, &allocation->phys_array_page_count);
}

static bool stack_fault_is_valid_locked(const struct vmm_alloc_record* allocation, size_t page_index) {
	size_t next_page;

	if (!allocation_is_stack_locked(allocation)) return true;
	if (page_index >= allocation->page_count) return false;
	if (allocation->mapped_page_count >= allocation->page_count) return false;

	next_page = allocation->page_count - allocation->mapped_page_count - 1u;
	return page_index == next_page;
}

static void release_empty_backing_array_locked(struct vmm_alloc_record* allocation) {
	if (!allocation || !allocation->phys_pages || allocation->mapped_page_count != 0) return;

	for (size_t page = 0; page < allocation->page_count; page++) {
		if (page_entry_has_backing(allocation->phys_pages[page])) return;
	}

	free_metadata_buffer(allocation->phys_array_phys, allocation->phys_array_page_count);
	allocation->phys_pages            = NULL;
	allocation->phys_array_phys       = 0;
	allocation->phys_array_page_count = 0;
}

static void restore_live_mappings_locked(struct address_space* space, const struct vmm_alloc_record* allocation,
                                         vmm_prot_t prot, bool replace_existing) {
	uint64_t                  flags;
	struct hal_address_space* hal_space;

	if (!allocation || !allocation->phys_pages) return;
	hal_space = vmm_hal_space(space);
	if (hal_space == NULL) return;
	flags = vmm_prot_to_hal_flags(prot);

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry         = allocation->phys_pages[page];
		uintptr_t virt          = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t existing_phys = 0;
		uintptr_t phys;

		if (!page_entry_is_mapped(entry)) continue;
		phys = page_entry_phys(entry);

		if (hal_paging_query(hal_space, virt, &existing_phys, NULL)) {
			if (!replace_existing) continue;
			if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) continue;
			(void)hal_paging_unmap(hal_space, virt);
		}
		(void)hal_paging_map(hal_space, virt, phys, flags);
	}
}

static bool map_page_locked(struct address_space* space, struct vmm_alloc_record* allocation, size_t page_index) {
	uintptr_t                 entry;
	uintptr_t                 phys;
	uintptr_t                 virt;
	uintptr_t                 existing_phys  = 0;
	bool                      allocated_phys = false;
	struct hal_address_space* hal_space;

	if (!allocation || page_index >= allocation->page_count) return false;
	hal_space = vmm_hal_space(space);
	if (hal_space == NULL) return false;
	if (!ensure_backing_array_locked(allocation)) return false;

	entry = allocation->phys_pages[page_index];
	if (page_entry_is_mapped(entry)) return true;

	phys = page_entry_phys(entry);
	if (phys == 0) {
		if (!pmm_alloc_pages(1, &phys)) return false;
		entry          = make_page_entry(phys, page_entry_flags(entry));
		allocated_phys = true;
	}

	virt = allocation->base + page_index * (uintptr_t)PMM_PAGE_SIZE;
	if (hal_paging_query(hal_space, virt, &existing_phys, NULL)) {
		if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) {
			if (allocated_phys) (void)pmm_free_pages(phys, 1);
			release_empty_backing_array_locked(allocation);
			return false;
		}
		allocation->phys_pages[page_index] = make_page_entry(phys, page_entry_flags(entry) | VMM_PAGE_ENTRY_MAPPED);
		allocation->mapped_page_count++;
		return true;
	}

	if (!hal_paging_map(hal_space, virt, phys, vmm_prot_to_hal_flags(allocation->prot))) {
		if (allocated_phys) (void)pmm_free_pages(phys, 1);
		release_empty_backing_array_locked(allocation);
		return false;
	}

	allocation->phys_pages[page_index] = make_page_entry(phys, page_entry_flags(entry) | VMM_PAGE_ENTRY_MAPPED);
	allocation->mapped_page_count++;
	return true;
}

static bool map_allocation_locked(struct address_space* space, struct vmm_alloc_record* allocation) {
	if (!allocation) return false;
	if (allocation->page_count == 0) return false;
	if (allocation_state_locked(allocation) == VMM_STATE_MAPPED) return true;
	if (vmm_hal_space(space) == NULL) return false;

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry;

		if (!ensure_backing_array_locked(allocation)) goto rollback;

		entry = allocation->phys_pages[page];
		if (page_entry_is_mapped(entry)) {
			allocation->phys_pages[page] = entry | VMM_PAGE_ENTRY_ROLLBACK_SKIP;
			continue;
		}
		if (page_entry_has_backing(entry)) {
			allocation->phys_pages[page] =
				make_page_entry(page_entry_phys(entry), page_entry_flags(entry) | VMM_PAGE_ENTRY_ROLLBACK_KEEP);
		}
		if (!map_page_locked(space, allocation, page)) goto rollback;
	}

	for (size_t page = 0; page < allocation->page_count; page++) {
		if (!allocation->phys_pages) break;
		allocation->phys_pages[page] &= ~(VMM_PAGE_ENTRY_ROLLBACK_KEEP | VMM_PAGE_ENTRY_ROLLBACK_SKIP);
	}
	return true;

rollback:
	if (allocation->phys_pages != NULL) {
		for (size_t page = 0; page < allocation->page_count; page++) {
			uintptr_t entry = allocation->phys_pages[page];

			if ((page_entry_flags(entry) & VMM_PAGE_ENTRY_ROLLBACK_SKIP) != 0) {
				allocation->phys_pages[page] &= ~VMM_PAGE_ENTRY_ROLLBACK_SKIP;
				continue;
			}
			if (!page_entry_is_mapped(entry)) continue;
			if ((page_entry_flags(entry) & VMM_PAGE_ENTRY_ROLLBACK_KEEP) != 0) {
				(void)hal_paging_unmap(vmm_hal_space(space), allocation->base + page * (uintptr_t)PMM_PAGE_SIZE);
				allocation->phys_pages[page] = make_page_entry(page_entry_phys(entry), 0);
				allocation->mapped_page_count--;
				continue;
			}

			(void)hal_paging_unmap(vmm_hal_space(space), allocation->base + page * (uintptr_t)PMM_PAGE_SIZE);
			(void)pmm_free_pages(page_entry_phys(entry), 1);
			allocation->phys_pages[page] = 0;
			allocation->mapped_page_count--;
		}
	}
	release_empty_backing_array_locked(allocation);
	return false;
}

static bool map_allocation_for_fault_locked(struct address_space* space, struct vmm_alloc_record* allocation,
                                            uintptr_t fault_addr) {
	size_t    page_index;
	uintptr_t entry = 0;

	if (!allocation || (allocation->map_flags & (uint64_t)VMM_MAP_LAZY) == 0) return false;
	if ((uint64_t)fault_addr < (uint64_t)allocation->base) return false;

	page_index = ((uintptr_t)fault_addr - allocation->base) / (uintptr_t)PMM_PAGE_SIZE;
	if (page_index >= allocation->page_count) return false;
	if (!stack_fault_is_valid_locked(allocation, page_index)) return false;
	if (allocation->phys_pages != NULL) entry = allocation->phys_pages[page_index];
	if (page_entry_is_mapped(entry)) return false;
	return map_page_locked(space, allocation, page_index);
}

static bool unmap_allocation_locked(struct address_space* space, struct vmm_alloc_record* allocation,
                                    bool release_phys) {
	struct hal_address_space* hal_space = vmm_hal_space(space);

	if (!allocation || allocation->mapped_page_count == 0 || !allocation->phys_pages) return false;
	if (hal_space == NULL) return false;

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry = allocation->phys_pages[page];
		uintptr_t virt  = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys  = 0;

		if (!page_entry_is_mapped(entry)) continue;
		if (!hal_paging_query(hal_space, virt, &phys, NULL)) {
			restore_live_mappings_locked(space, allocation, allocation->prot, false);
			return false;
		}
		if ((phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != page_entry_phys(entry)) {
			restore_live_mappings_locked(space, allocation, allocation->prot, false);
			return false;
		}
		if (!hal_paging_unmap(hal_space, virt)) {
			restore_live_mappings_locked(space, allocation, allocation->prot, false);
			return false;
		}
	}

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry = allocation->phys_pages[page];

		if (!page_entry_is_mapped(entry)) continue;
		allocation->phys_pages[page] =
			make_page_entry(page_entry_phys(entry), page_entry_flags(entry) & ~VMM_PAGE_ENTRY_MAPPED);
	}

	allocation->mapped_page_count = 0;
	if (release_phys) {
		release_allocation_backing_locked(allocation);
		return true;
	}
	return true;
}

static bool protect_allocation_locked(struct address_space* space, struct vmm_alloc_record* allocation,
                                      vmm_prot_t new_prot) {
	vmm_prot_t                old_prot;
	uint64_t                  new_flags;
	enum vmm_state            old_state;
	struct hal_address_space* hal_space;

	if (!allocation) return false;
	hal_space = vmm_hal_space(space);
	if (hal_space == NULL) return false;
	if (allocation->prot == new_prot) return true;

	old_prot  = allocation->prot;
	old_state = allocation_state_locked(allocation);
	if (old_state == VMM_STATE_RESERVED) {
		allocation->prot = new_prot;
		return true;
	}
	if (!allocation->phys_pages) return false;

	new_flags = vmm_prot_to_hal_flags(new_prot);

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry = allocation->phys_pages[page];
		uintptr_t virt  = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys;

		if (!page_entry_is_mapped(entry)) continue;
		phys = page_entry_phys(entry);
		if (!hal_paging_unmap(hal_space, virt)) {
			restore_live_mappings_locked(space, allocation, old_prot, true);
			return false;
		}
		if (!hal_paging_map(hal_space, virt, phys, new_flags)) {
			restore_live_mappings_locked(space, allocation, old_prot, true);
			return false;
		}
	}

	allocation->prot = new_prot;
	return true;
}

static void fill_allocation_info_locked(const struct vmm_alloc_record* allocation, struct vmm_info* out_info) {
	if (!allocation || !out_info) return;

	*out_info = (struct vmm_info){
		.id          = allocation->id,
		.base        = (void*)allocation->base,
		.page_count  = allocation->page_count,
		.prot        = allocation->prot,
		.kind        = allocation->kind,
		.guard_pages = allocation->guard_pages,
		.state       = allocation_state_locked(allocation),
		.first_phys  = allocation->phys_pages ? page_entry_phys(allocation->phys_pages[0]) : 0,
	};
}

static void reset_allocations_locked(struct address_space* space) {
	if (!space) return;
	if (space->allocations != NULL) {
		for (size_t i = 0; i < space->allocations_capacity; i++) {
			if (!space->allocations[i].used) continue;

			if (space->allocations[i].phys_pages != NULL) {
				for (size_t page = 0; page < space->allocations[i].page_count; page++) {
					uintptr_t virt = space->allocations[i].base + page * (uintptr_t)PMM_PAGE_SIZE;

					if (page_entry_is_mapped(space->allocations[i].phys_pages[page]))
						(void)hal_paging_unmap(vmm_hal_space(space), virt);
				}
			}
			release_allocation_backing_locked(&space->allocations[i]);
			space->allocations[i].used = false;
		}

		free_metadata_buffer(space->allocations_phys, space->allocations_page_count);
	}

	space->allocations            = NULL;
	space->allocations_phys       = 0;
	space->allocations_page_count = 0;
	space->allocations_capacity   = 0;
	space->allocation_count       = 0;
	space->next_allocation_id     = 1u;
}

bool vmm_init(void) {
	size_t           window_pages = VMM_WINDOW_SIZE / PMM_PAGE_SIZE;
	struct irq_state state;

	state = spinlock_lock_irqsave(&vmm_lock);
	reset_allocations_locked(address_space_kernel());
	if (!hal_paging_init()) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (!address_space_init(address_space_kernel(), (uintptr_t)VMM_WINDOW_BASE, window_pages)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (hal_paging_kernel_space() == NULL) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	address_space_kernel()->hal_space = *hal_paging_kernel_space();
	if (!ensure_allocation_capacity_locked(address_space_kernel())) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	initialized = true;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_is_initialized(void) {
	return initialized;
}

bool vmm_alloc(struct address_space* space, const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;
	uintptr_t                reserved_base = 0;
	uintptr_t                base          = 0;
	size_t                   align_pages;
	size_t                   guard_pages;
	size_t                   reserved_page_count;

	if (out_id) *out_id = VMM_ID_INVALID;
	if (out_base) *out_base = NULL;

	if (!initialized || !space || !params || (!out_id && !out_base) || params->page_count == 0) return false;
	if (!address_space_is_initialized(space)) return false;
	if (!vmm_prot_is_valid(params->prot)) return false;
	if (!vmm_params_allowed_for_space(space, params)) return false;
	if ((params->map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0) return false;

	align_pages = params->align_pages != 0 ? params->align_pages : VMM_MIN_ALIGN_PAGES;
	if ((align_pages & (align_pages - 1u)) != 0) return false;
	guard_pages = 0;
	if (params->kind == VMM_KIND_STACK) {
		guard_pages = params->guard_pages != 0 ? params->guard_pages : VMM_STACK_DEFAULT_GUARD_PAGES;
		if ((guard_pages % align_pages) != 0) return false;
	}
	else if (params->guard_pages != 0) {
		return false;
	}
	if (add_overflow_size(params->page_count, guard_pages, &reserved_page_count)) return false;
	if (reserved_page_count == 0) return false;

	state = spinlock_lock_irqsave(&vmm_lock);
	if (!address_space_reserve(space, reserved_page_count, align_pages, &reserved_base)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	base = reserved_base + guard_pages * (uintptr_t)PMM_PAGE_SIZE;
	if (!address_space_range_contains(space, reserved_base, reserved_page_count) ||
	    !ensure_allocation_capacity_locked(space)) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}

	allocation = find_free_allocation_slot_locked(space);
	if (!allocation) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}

	*allocation = (struct vmm_alloc_record){
		.id                  = space->next_allocation_id++,
		.reserved_base       = reserved_base,
		.base                = base,
		.reserved_page_count = reserved_page_count,
		.page_count          = params->page_count,
		.prot                = params->prot,
		.kind                = params->kind,
		.guard_pages         = guard_pages,
		.map_flags           = params->map_flags,
		.used                = true,
	};

	if ((params->map_flags & VMM_MAP_LAZY) == 0 && !map_allocation_locked(space, allocation)) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		memset(allocation, 0, sizeof(*allocation));
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}

	space->allocation_count++;
	if (out_id) *out_id = allocation->id;
	if (out_base) *out_base = (void*)base;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_free(struct address_space* space, vmm_id_t id) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_id_locked(space, id);
	if (!allocation) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (allocation->mapped_page_count != 0 && !unmap_allocation_locked(space, allocation, false)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)address_space_release(space, allocation->reserved_base, allocation->reserved_page_count);
	memset(allocation, 0, sizeof(*allocation));
	space->allocation_count--;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_free_at(struct address_space* space, void* base) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;

	if (!initialized || !space || !base) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_base_locked(space, (uintptr_t)base);
	if (!allocation) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (allocation->mapped_page_count != 0 && !unmap_allocation_locked(space, allocation, false)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)address_space_release(space, allocation->reserved_base, allocation->reserved_page_count);
	memset(allocation, 0, sizeof(*allocation));
	space->allocation_count--;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_map(struct address_space* space, vmm_id_t id) {
	struct vmm_alloc_record* allocation;
	bool                     ok;
	struct irq_state         state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_id_locked(space, id);
	ok         = map_allocation_locked(space, allocation);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_unmap(struct address_space* space, vmm_id_t id, bool release_phys) {
	struct vmm_alloc_record* allocation;
	bool                     ok;
	struct irq_state         state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_id_locked(space, id);
	ok         = unmap_allocation_locked(space, allocation, release_phys);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_protect(struct address_space* space, vmm_id_t id, vmm_prot_t new_prot) {
	struct vmm_alloc_record* allocation;
	bool                     ok;
	struct irq_state         state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;
	if (!vmm_prot_is_valid(new_prot)) return false;
	if (address_space_is_kernel(space)) {
		if ((new_prot & VMM_PROT_USER) != 0) return false;
	}
	else {
		if ((new_prot & VMM_PROT_USER) == 0 || (new_prot & VMM_PROT_GLOBAL) != 0) return false;
	}

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_id_locked(space, id);
	ok         = protect_allocation_locked(space, allocation, new_prot);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_resolve_page_fault(struct address_space* space, uintptr_t addr) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;
	uintptr_t                page_base;
	bool                     ok = false;

	if (!initialized || !space) return false;
	page_base = addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	if (hal_paging_query(vmm_hal_space(space), page_base, NULL, NULL)) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_containing_locked(space, addr);
	ok         = map_allocation_for_fault_locked(space, allocation, addr);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_query(struct address_space* space, void* addr, struct vmm_info* out_info) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !space || !addr || !out_info) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_containing_locked(space, (uintptr_t)addr);
	if (allocation) fill_allocation_info_locked(allocation, out_info);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return allocation != NULL;
}

bool vmm_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info) {
	struct vmm_alloc_record* allocation;
	struct irq_state         state;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !space || id == VMM_ID_INVALID || !out_info) return false;

	state      = spinlock_lock_irqsave(&vmm_lock);
	allocation = find_allocation_by_id_locked(space, id);
	if (allocation) fill_allocation_info_locked(allocation, out_info);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return allocation != NULL;
}

uintptr_t vmm_window_base(void) {
	return (uintptr_t)VMM_WINDOW_BASE;
}

size_t vmm_window_page_count(void) {
	return VMM_WINDOW_SIZE / PMM_PAGE_SIZE;
}

size_t vmm_count(struct address_space* space) {
	size_t           count;
	struct irq_state state;

	if (!space) return 0u;
	state = spinlock_lock_irqsave(&vmm_lock);
	count = space->allocation_count;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return count;
}

bool vmm_user_address_space_init(struct address_space* space) {
	size_t                   page_count;
	struct hal_address_space hal_space;

	if (!initialized || !space) return false;
	page_count = MM_USER_VMM_SIZE / PMM_PAGE_SIZE;
	if (page_count == 0) return false;

	if (!hal_paging_space_create(&hal_space)) return false;
	if (!address_space_init(space, (uintptr_t)MM_USER_VMM_BASE, page_count)) {
		hal_paging_space_destroy(&hal_space);
		return false;
	}
	space->hal_space = hal_space;
	return true;
}

void vmm_address_space_deinit(struct address_space* space) {
	struct irq_state         state;
	struct hal_address_space hal_space;

	if (!space) return;

	state = spinlock_lock_irqsave(&vmm_lock);
	reset_allocations_locked(space);
	spinlock_unlock_irqrestore(&vmm_lock, state);

	hal_space = space->hal_space;
	address_space_deinit(space);
	if (space != address_space_kernel()) hal_paging_space_destroy(&hal_space);
}
