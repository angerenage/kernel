#include <core/math.h>
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

#define VMM_WINDOW_BASE 0xffffffffa0000000ull
#define VMM_WINDOW_SIZE 0x40000000ull
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

static struct vmm_alloc_record* allocations;
static uintptr_t                allocations_phys;
static size_t                   allocations_page_count;
static size_t                   allocations_capacity;
static size_t                   allocation_count;
static vmm_id_t                 next_allocation_id = 1u;
static bool                     initialized;
static struct spinlock          vmm_lock = SPINLOCK_INIT;

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static inline uint64_t vmm_prot_to_hal_flags(vmm_prot_t prot) {
	return (uint64_t)(prot & (VMM_PROT_WRITE | VMM_PROT_EXEC | VMM_PROT_GLOBAL | VMM_PROT_NO_CACHE));
}

static inline bool vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0;
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

static struct vmm_alloc_record* find_allocation_by_id_locked(vmm_id_t id) {
	for (size_t i = 0; i < allocations_capacity; i++) {
		if (!allocations[i].used) continue;
		if (allocations[i].id == id) return &allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_allocation_by_base_locked(uintptr_t base) {
	for (size_t i = 0; i < allocations_capacity; i++) {
		if (!allocations[i].used) continue;
		if (allocations[i].base == base) return &allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_allocation_containing_locked(uintptr_t addr) {
	for (size_t i = 0; i < allocations_capacity; i++) {
		uint64_t span;
		uint64_t end;

		if (!allocations[i].used) continue;
		if (mul_overflow_u64((uint64_t)allocations[i].page_count, PMM_PAGE_SIZE, &span)) continue;
		if (add_overflow_u64((uint64_t)allocations[i].base, span, &end)) continue;
		if ((uint64_t)addr < (uint64_t)allocations[i].base) continue;
		if ((uint64_t)addr >= end) continue;
		return &allocations[i];
	}

	return NULL;
}

static struct vmm_alloc_record* find_free_allocation_slot_locked(void) {
	for (size_t i = 0; i < allocations_capacity; i++) {
		if (!allocations[i].used) return &allocations[i];
	}

	return NULL;
}

static bool ensure_allocation_capacity_locked(void) {
	struct vmm_alloc_record* new_allocations;
	uintptr_t                new_allocations_phys;
	size_t                   new_allocations_page_count;
	size_t                   new_capacity;
	size_t                   bytes;

	if (find_free_allocation_slot_locked() != NULL) return true;

	new_capacity = allocations_capacity != 0 ? allocations_capacity * 2u : VMM_INITIAL_ALLOCATION_CAPACITY;
	if (new_capacity < allocations_capacity) return false;
	if (mul_overflow_size(new_capacity, sizeof(struct vmm_alloc_record), &bytes)) return false;
	if (!alloc_metadata_buffer(bytes, (void**)&new_allocations, &new_allocations_phys, &new_allocations_page_count)) {
		return false;
	}

	if (allocations != NULL && allocations_capacity != 0) {
		memcpy(new_allocations, allocations, allocations_capacity * sizeof(struct vmm_alloc_record));
		free_metadata_buffer(allocations_phys, allocations_page_count);
	}

	allocations            = new_allocations;
	allocations_phys       = new_allocations_phys;
	allocations_page_count = new_allocations_page_count;
	allocations_capacity   = new_capacity;
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

static void restore_live_mappings_locked(const struct vmm_alloc_record* allocation, vmm_prot_t prot,
                                         bool replace_existing) {
	uint64_t flags;

	if (!allocation || !allocation->phys_pages) return;
	flags = vmm_prot_to_hal_flags(prot);

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry         = allocation->phys_pages[page];
		uintptr_t virt          = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t existing_phys = 0;
		uintptr_t phys;

		if (!page_entry_is_mapped(entry)) continue;
		phys = page_entry_phys(entry);

		if (hal_paging_query(virt, &existing_phys, NULL)) {
			if (!replace_existing) continue;
			if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) continue;
			(void)hal_paging_unmap(virt);
		}
		(void)hal_paging_map(virt, phys, flags);
	}
}

static bool map_page_locked(struct vmm_alloc_record* allocation, size_t page_index) {
	uintptr_t entry;
	uintptr_t phys;
	uintptr_t virt;
	uintptr_t existing_phys  = 0;
	bool      allocated_phys = false;

	if (!allocation || page_index >= allocation->page_count) return false;
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
	if (hal_paging_query(virt, &existing_phys, NULL)) {
		if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) {
			if (allocated_phys) (void)pmm_free_pages(phys, 1);
			release_empty_backing_array_locked(allocation);
			return false;
		}
		allocation->phys_pages[page_index] = make_page_entry(phys, page_entry_flags(entry) | VMM_PAGE_ENTRY_MAPPED);
		allocation->mapped_page_count++;
		return true;
	}

	if (!hal_paging_map(virt, phys, vmm_prot_to_hal_flags(allocation->prot))) {
		if (allocated_phys) (void)pmm_free_pages(phys, 1);
		release_empty_backing_array_locked(allocation);
		return false;
	}

	allocation->phys_pages[page_index] = make_page_entry(phys, page_entry_flags(entry) | VMM_PAGE_ENTRY_MAPPED);
	allocation->mapped_page_count++;
	return true;
}

static bool map_allocation_locked(struct vmm_alloc_record* allocation) {
	if (!allocation) return false;
	if (allocation->page_count == 0) return false;
	if (allocation_state_locked(allocation) == VMM_STATE_MAPPED) return true;

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
		if (!map_page_locked(allocation, page)) goto rollback;
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
				(void)hal_paging_unmap(allocation->base + page * (uintptr_t)PMM_PAGE_SIZE);
				allocation->phys_pages[page] = make_page_entry(page_entry_phys(entry), 0);
				allocation->mapped_page_count--;
				continue;
			}

			(void)hal_paging_unmap(allocation->base + page * (uintptr_t)PMM_PAGE_SIZE);
			(void)pmm_free_pages(page_entry_phys(entry), 1);
			allocation->phys_pages[page] = 0;
			allocation->mapped_page_count--;
		}
	}
	release_empty_backing_array_locked(allocation);
	return false;
}

static bool map_allocation_for_fault_locked(struct vmm_alloc_record* allocation, uintptr_t fault_addr) {
	size_t    page_index;
	uintptr_t entry = 0;

	if (!allocation || (allocation->map_flags & (uint64_t)VMM_MAP_LAZY) == 0) return false;
	if ((uint64_t)fault_addr < (uint64_t)allocation->base) return false;

	page_index = ((uintptr_t)fault_addr - allocation->base) / (uintptr_t)PMM_PAGE_SIZE;
	if (page_index >= allocation->page_count) return false;
	if (!stack_fault_is_valid_locked(allocation, page_index)) return false;
	if (allocation->phys_pages != NULL) entry = allocation->phys_pages[page_index];
	if (page_entry_is_mapped(entry)) return false;
	return map_page_locked(allocation, page_index);
}

static bool unmap_allocation_locked(struct vmm_alloc_record* allocation, bool release_phys) {
	if (!allocation || allocation->mapped_page_count == 0 || !allocation->phys_pages) return false;

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t entry = allocation->phys_pages[page];
		uintptr_t virt  = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys  = 0;

		if (!page_entry_is_mapped(entry)) continue;
		if (!hal_paging_query(virt, &phys, NULL)) {
			restore_live_mappings_locked(allocation, allocation->prot, false);
			return false;
		}
		if ((phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != page_entry_phys(entry)) {
			restore_live_mappings_locked(allocation, allocation->prot, false);
			return false;
		}
		if (!hal_paging_unmap(virt)) {
			restore_live_mappings_locked(allocation, allocation->prot, false);
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

static bool protect_allocation_locked(struct vmm_alloc_record* allocation, vmm_prot_t new_prot) {
	vmm_prot_t     old_prot;
	uint64_t       new_flags;
	enum vmm_state old_state;

	if (!allocation) return false;
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
		if (!hal_paging_unmap(virt)) {
			restore_live_mappings_locked(allocation, old_prot, true);
			return false;
		}
		if (!hal_paging_map(virt, phys, new_flags)) {
			restore_live_mappings_locked(allocation, old_prot, true);
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

static void reset_allocations_locked(void) {
	if (allocations != NULL) {
		for (size_t i = 0; i < allocations_capacity; i++) {
			if (!allocations[i].used) continue;

			if (allocations[i].phys_pages != NULL) {
				for (size_t page = 0; page < allocations[i].page_count; page++) {
					uintptr_t virt = allocations[i].base + page * (uintptr_t)PMM_PAGE_SIZE;

					if (page_entry_is_mapped(allocations[i].phys_pages[page])) (void)hal_paging_unmap(virt);
				}
			}
			release_allocation_backing_locked(&allocations[i]);
			allocations[i].used = false;
		}

		free_metadata_buffer(allocations_phys, allocations_page_count);
	}

	allocations            = NULL;
	allocations_phys       = 0;
	allocations_page_count = 0;
	allocations_capacity   = 0;
	allocation_count       = 0;
	next_allocation_id     = 1u;
	initialized            = false;
}

bool vmm_init(void) {
	size_t window_pages = VMM_WINDOW_SIZE / PMM_PAGE_SIZE;

	spinlock_lock(&vmm_lock);
	reset_allocations_locked();
	if (!hal_paging_init()) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (!vaddr_alloc_init((uintptr_t)VMM_WINDOW_BASE, window_pages)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (!ensure_allocation_capacity_locked()) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	initialized = true;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_is_initialized(void) {
	return initialized;
}

bool vmm_alloc(const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base) {
	struct vmm_alloc_record* allocation;
	uintptr_t                reserved_base = 0;
	uintptr_t                base          = 0;
	size_t                   align_pages;
	size_t                   guard_pages;
	size_t                   reserved_page_count;

	if (out_id) *out_id = VMM_ID_INVALID;
	if (out_base) *out_base = NULL;

	if (!initialized || !params || (!out_id && !out_base) || params->page_count == 0) return false;
	if (!vmm_prot_is_valid(params->prot)) return false;
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

	spinlock_lock(&vmm_lock);
	if (!vaddr_alloc_reserve(reserved_page_count, align_pages, &reserved_base)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	base = reserved_base + guard_pages * (uintptr_t)PMM_PAGE_SIZE;
	if (!ensure_allocation_capacity_locked()) {
		(void)vaddr_alloc_release(reserved_base, reserved_page_count);
		spinlock_unlock(&vmm_lock);
		return false;
	}

	allocation = find_free_allocation_slot_locked();
	if (!allocation) {
		(void)vaddr_alloc_release(reserved_base, reserved_page_count);
		spinlock_unlock(&vmm_lock);
		return false;
	}

	*allocation = (struct vmm_alloc_record){
		.id                  = next_allocation_id++,
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

	if ((params->map_flags & VMM_MAP_LAZY) == 0 && !map_allocation_locked(allocation)) {
		(void)vaddr_alloc_release(reserved_base, reserved_page_count);
		memset(allocation, 0, sizeof(*allocation));
		spinlock_unlock(&vmm_lock);
		return false;
	}

	allocation_count++;
	if (out_id) *out_id = allocation->id;
	if (out_base) *out_base = (void*)base;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_free(vmm_id_t id) {
	struct vmm_alloc_record* allocation;

	if (!initialized || id == VMM_ID_INVALID) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_id_locked(id);
	if (!allocation) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (allocation->mapped_page_count != 0 && !unmap_allocation_locked(allocation, false)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)vaddr_alloc_release(allocation->reserved_base, allocation->reserved_page_count);
	memset(allocation, 0, sizeof(*allocation));
	allocation_count--;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_free_at(void* base) {
	struct vmm_alloc_record* allocation;

	if (!initialized || !base) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_base_locked((uintptr_t)base);
	if (!allocation) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (allocation->mapped_page_count != 0 && !unmap_allocation_locked(allocation, false)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)vaddr_alloc_release(allocation->reserved_base, allocation->reserved_page_count);
	memset(allocation, 0, sizeof(*allocation));
	allocation_count--;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_map(vmm_id_t id) {
	struct vmm_alloc_record* allocation;
	bool                     ok;

	if (!initialized || id == VMM_ID_INVALID) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_id_locked(id);
	ok         = map_allocation_locked(allocation);
	spinlock_unlock(&vmm_lock);
	return ok;
}

bool vmm_unmap(vmm_id_t id, bool release_phys) {
	struct vmm_alloc_record* allocation;
	bool                     ok;

	if (!initialized || id == VMM_ID_INVALID) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_id_locked(id);
	ok         = unmap_allocation_locked(allocation, release_phys);
	spinlock_unlock(&vmm_lock);
	return ok;
}

bool vmm_protect(vmm_id_t id, vmm_prot_t new_prot) {
	struct vmm_alloc_record* allocation;
	bool                     ok;

	if (!initialized || id == VMM_ID_INVALID) return false;
	if (!vmm_prot_is_valid(new_prot)) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_id_locked(id);
	ok         = protect_allocation_locked(allocation, new_prot);
	spinlock_unlock(&vmm_lock);
	return ok;
}

bool vmm_resolve_page_fault(uintptr_t addr) {
	struct vmm_alloc_record* allocation;
	uintptr_t                page_base;
	bool                     ok = false;

	if (!initialized) return false;
	page_base = addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	if (hal_paging_query(page_base, NULL, NULL)) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_containing_locked(addr);
	ok         = map_allocation_for_fault_locked(allocation, addr);
	spinlock_unlock(&vmm_lock);
	return ok;
}

bool vmm_query(void* addr, struct vmm_info* out_info) {
	struct vmm_alloc_record* allocation;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !addr || !out_info) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_containing_locked((uintptr_t)addr);
	if (allocation) fill_allocation_info_locked(allocation, out_info);
	spinlock_unlock(&vmm_lock);
	return allocation != NULL;
}

bool vmm_query_id(vmm_id_t id, struct vmm_info* out_info) {
	struct vmm_alloc_record* allocation;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || id == VMM_ID_INVALID || !out_info) return false;

	spinlock_lock(&vmm_lock);
	allocation = find_allocation_by_id_locked(id);
	if (allocation) fill_allocation_info_locked(allocation, out_info);
	spinlock_unlock(&vmm_lock);
	return allocation != NULL;
}

uintptr_t vmm_window_base(void) {
	return (uintptr_t)VMM_WINDOW_BASE;
}

size_t vmm_window_page_count(void) {
	return VMM_WINDOW_SIZE / PMM_PAGE_SIZE;
}

size_t vmm_count(void) {
	size_t count;

	spinlock_lock(&vmm_lock);
	count = allocation_count;
	spinlock_unlock(&vmm_lock);
	return count;
}
