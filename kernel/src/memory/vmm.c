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

struct vmm_alloc_record {
	vmm_id_t       id;
	uintptr_t      base;
	size_t         page_count;
	vmm_prot_t     prot;
	enum vmm_kind  kind;
	enum vmm_state state;
	uintptr_t*     phys_pages;
	uintptr_t      phys_array_phys;
	size_t         phys_array_page_count;
	bool           used;
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
		(void)pmm_free_pages(allocation->phys_pages[page], 1);
	}

	free_metadata_buffer(allocation->phys_array_phys, allocation->phys_array_page_count);
	allocation->phys_pages            = NULL;
	allocation->phys_array_phys       = 0;
	allocation->phys_array_page_count = 0;
}

static void restore_mapped_pages_locked(const struct vmm_alloc_record* allocation, size_t mapped_pages, vmm_prot_t prot,
                                        bool replace_existing) {
	uint64_t flags;

	if (!allocation || !allocation->phys_pages) return;
	flags = vmm_prot_to_hal_flags(prot);

	for (size_t page = 0; page < mapped_pages; page++) {
		uintptr_t virt          = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t existing_phys = 0;

		if (hal_paging_query(virt, &existing_phys, NULL)) {
			if (!replace_existing) continue;
			if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != allocation->phys_pages[page]) continue;
			(void)hal_paging_unmap(virt);
		}
		(void)hal_paging_map(virt, allocation->phys_pages[page], flags);
	}
}

static bool map_allocation_locked(struct vmm_alloc_record* allocation) {
	uintptr_t* phys_pages                 = allocation ? allocation->phys_pages : NULL;
	uintptr_t  phys_array_phys            = 0;
	size_t     phys_array_page_count      = 0;
	size_t     newly_allocated_phys_pages = 0;
	bool       owns_new_backing           = false;
	uint64_t   flags;

	if (!allocation || allocation->state != VMM_STATE_RESERVED) return false;

	flags = vmm_prot_to_hal_flags(allocation->prot);

	if (!phys_pages) {
		size_t bytes;

		if (mul_overflow_size(allocation->page_count, sizeof(uintptr_t), &bytes)) return false;
		if (!alloc_metadata_buffer(bytes, (void**)&phys_pages, &phys_array_phys, &phys_array_page_count)) return false;

		owns_new_backing = true;
		for (size_t page = 0; page < allocation->page_count; page++) {
			if (!pmm_alloc_pages(1, &phys_pages[page])) {
				for (size_t allocated = 0; allocated < newly_allocated_phys_pages; allocated++) {
					(void)pmm_free_pages(phys_pages[allocated], 1);
				}
				free_metadata_buffer(phys_array_phys, phys_array_page_count);
				return false;
			}
			newly_allocated_phys_pages++;
		}
	}

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t virt = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;

		if (!hal_paging_map(virt, phys_pages[page], flags)) {
			for (size_t mapped = 0; mapped < page; mapped++) {
				uintptr_t rollback_virt = allocation->base + mapped * (uintptr_t)PMM_PAGE_SIZE;

				(void)hal_paging_unmap(rollback_virt);
			}
			if (owns_new_backing) {
				for (size_t allocated = 0; allocated < newly_allocated_phys_pages; allocated++) {
					(void)pmm_free_pages(phys_pages[allocated], 1);
				}
				free_metadata_buffer(phys_array_phys, phys_array_page_count);
			}
			return false;
		}
	}

	if (owns_new_backing) {
		allocation->phys_pages            = phys_pages;
		allocation->phys_array_phys       = phys_array_phys;
		allocation->phys_array_page_count = phys_array_page_count;
	}

	allocation->state = VMM_STATE_MAPPED;
	return true;
}

static bool unmap_allocation_locked(struct vmm_alloc_record* allocation, bool release_phys) {
	size_t unmapped_pages = 0;

	if (!allocation || allocation->state != VMM_STATE_MAPPED || !allocation->phys_pages) return false;

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t virt = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys = 0;

		if (!hal_paging_query(virt, &phys, NULL)) {
			restore_mapped_pages_locked(allocation, unmapped_pages, allocation->prot, false);
			return false;
		}
		if ((phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != allocation->phys_pages[page]) {
			restore_mapped_pages_locked(allocation, unmapped_pages, allocation->prot, false);
			return false;
		}
		if (!hal_paging_unmap(virt)) {
			restore_mapped_pages_locked(allocation, unmapped_pages, allocation->prot, false);
			return false;
		}
		unmapped_pages++;
	}

	allocation->state = VMM_STATE_RESERVED;
	if (release_phys) release_allocation_backing_locked(allocation);
	return true;
}

static bool protect_allocation_locked(struct vmm_alloc_record* allocation, vmm_prot_t new_prot) {
	size_t     remapped_pages = 0;
	vmm_prot_t old_prot;
	uint64_t   new_flags;

	if (!allocation) return false;
	if (allocation->prot == new_prot) return true;

	old_prot = allocation->prot;
	if (allocation->state == VMM_STATE_RESERVED) {
		allocation->prot = new_prot;
		return true;
	}
	if (allocation->state != VMM_STATE_MAPPED || !allocation->phys_pages) return false;

	new_flags = vmm_prot_to_hal_flags(new_prot);

	for (size_t page = 0; page < allocation->page_count; page++) {
		uintptr_t virt = allocation->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys = allocation->phys_pages[page];

		if (!hal_paging_unmap(virt)) {
			restore_mapped_pages_locked(allocation, remapped_pages, old_prot, true);
			return false;
		}
		if (!hal_paging_map(virt, phys, new_flags)) {
			restore_mapped_pages_locked(allocation, page + 1u, old_prot, true);
			return false;
		}
		remapped_pages++;
	}

	allocation->prot = new_prot;
	return true;
}

static void fill_allocation_info_locked(const struct vmm_alloc_record* allocation, struct vmm_info* out_info) {
	if (!allocation || !out_info) return;

	*out_info = (struct vmm_info){
		.id         = allocation->id,
		.base       = (void*)allocation->base,
		.page_count = allocation->page_count,
		.prot       = allocation->prot,
		.kind       = allocation->kind,
		.state      = allocation->state,
		.first_phys = allocation->phys_pages ? allocation->phys_pages[0] : 0,
	};
}

static void reset_allocations_locked(void) {
	if (allocations != NULL) {
		for (size_t i = 0; i < allocations_capacity; i++) {
			if (!allocations[i].used) continue;

			if (allocations[i].state == VMM_STATE_MAPPED) {
				for (size_t page = 0; page < allocations[i].page_count; page++) {
					uintptr_t virt = allocations[i].base + page * (uintptr_t)PMM_PAGE_SIZE;

					(void)hal_paging_unmap(virt);
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
	uintptr_t                base = 0;
	size_t                   align_pages;

	if (out_id) *out_id = VMM_ID_INVALID;
	if (out_base) *out_base = NULL;

	if (!initialized || !params || (!out_id && !out_base) || params->page_count == 0) return false;
	if (!vmm_prot_is_valid(params->prot)) return false;
	if ((params->map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0) return false;

	align_pages = params->align_pages != 0 ? params->align_pages : VMM_MIN_ALIGN_PAGES;
	if ((align_pages & (align_pages - 1u)) != 0) return false;

	spinlock_lock(&vmm_lock);
	if (!vaddr_alloc_reserve(params->page_count, align_pages, &base)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (!ensure_allocation_capacity_locked()) {
		(void)vaddr_alloc_release(base, params->page_count);
		spinlock_unlock(&vmm_lock);
		return false;
	}

	allocation = find_free_allocation_slot_locked();
	if (!allocation) {
		(void)vaddr_alloc_release(base, params->page_count);
		spinlock_unlock(&vmm_lock);
		return false;
	}

	*allocation = (struct vmm_alloc_record){
		.id         = next_allocation_id++,
		.base       = base,
		.page_count = params->page_count,
		.prot       = params->prot,
		.kind       = params->kind,
		.state      = VMM_STATE_RESERVED,
		.used       = true,
	};

	if ((params->map_flags & VMM_MAP_LAZY) == 0 && !map_allocation_locked(allocation)) {
		(void)vaddr_alloc_release(base, params->page_count);
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
	if (allocation->state == VMM_STATE_MAPPED && !unmap_allocation_locked(allocation, false)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)vaddr_alloc_release(allocation->base, allocation->page_count);
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
	if (allocation->state == VMM_STATE_MAPPED && !unmap_allocation_locked(allocation, false)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	release_allocation_backing_locked(allocation);
	(void)vaddr_alloc_release(allocation->base, allocation->page_count);
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
