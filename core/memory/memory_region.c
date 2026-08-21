#include <base/math.h>
#include <core/memory_region.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MEMORY_REGION_INITIAL_CAPACITY 16u

static inline bool space_is_kernel(const struct address_space* space) {
	return space != NULL && space == address_space_kernel();
}

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static bool alloc_metadata(size_t bytes, void** out_virt, uintptr_t* out_phys, size_t* out_pages) {
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

static void free_metadata(uintptr_t phys, size_t pages) {
	if (pages == 0) return;
	(void)pmm_free_pages(phys, pages);
}

static bool range_contains(const struct address_space* space, uintptr_t base, size_t page_count) {
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

bool memory_region_params_allowed(const struct address_space* space, const struct vmm_alloc_params* params) {
	if (!space || !params) return false;
	if (space_is_kernel(space)) return (params->prot & VMM_PROT_USER) == 0;
	if ((params->prot & VMM_PROT_USER) == 0) return false;
	if ((params->prot & VMM_PROT_GLOBAL) != 0) return false;
	switch (params->kind) {
	case VMM_KIND_GENERIC:
	case VMM_KIND_HEAP:
	case VMM_KIND_STACK:
	case VMM_KIND_PHYSICAL:
		return true;
	case VMM_KIND_MMIO:
	case VMM_KIND_KERNEL_TEXT:
	case VMM_KIND_KERNEL_RODATA:
	case VMM_KIND_KERNEL_DATA:
	default:
		return false;
	}
}

struct memory_region* memory_region_find_by_id(struct address_space* space, vmm_id_t id) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->regions_capacity; i++) {
		if (!space->regions[i].used) continue;
		if (space->regions[i].id == id) return &space->regions[i];
	}
	return NULL;
}

struct memory_region* memory_region_find_by_base(struct address_space* space, uintptr_t base) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->regions_capacity; i++) {
		if (!space->regions[i].used) continue;
		if (space->regions[i].base == base) return &space->regions[i];
	}
	return NULL;
}

struct memory_region* memory_region_find_containing(struct address_space* space, uintptr_t addr) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->regions_capacity; i++) {
		uint64_t span;
		uint64_t end;

		if (!space->regions[i].used) continue;
		if (mul_overflow_u64((uint64_t)space->regions[i].page_count, PMM_PAGE_SIZE, &span)) continue;
		if (add_overflow_u64((uint64_t)space->regions[i].base, span, &end)) continue;
		if ((uint64_t)addr < (uint64_t)space->regions[i].base) continue;
		if ((uint64_t)addr >= end) continue;
		return &space->regions[i];
	}
	return NULL;
}

static struct memory_region* find_free_slot(struct address_space* space) {
	if (!space) return NULL;
	for (size_t i = 0; i < space->regions_capacity; i++) {
		if (!space->regions[i].used) return &space->regions[i];
	}
	return NULL;
}

bool memory_region_ensure_capacity(struct address_space* space) {
	struct memory_region* new_regions;
	uintptr_t             new_regions_phys;
	size_t                new_regions_page_count;
	size_t                new_capacity;
	size_t                bytes;

	if (!space) return false;
	if (find_free_slot(space) != NULL) return true;
	new_capacity = space->regions_capacity != 0 ? space->regions_capacity * 2u : MEMORY_REGION_INITIAL_CAPACITY;
	if (new_capacity < space->regions_capacity) return false;
	if (mul_overflow_size(new_capacity, sizeof(struct memory_region), &bytes)) return false;
	if (!alloc_metadata(bytes, (void**)&new_regions, &new_regions_phys, &new_regions_page_count)) return false;
	if (space->regions != NULL && space->regions_capacity != 0) {
		memcpy(new_regions, space->regions, space->regions_capacity * sizeof(struct memory_region));
		free_metadata(space->regions_phys, space->regions_page_count);
	}
	space->regions            = new_regions;
	space->regions_phys       = new_regions_phys;
	space->regions_page_count = new_regions_page_count;
	space->regions_capacity   = new_capacity;
	if (space->next_region_id == 0u) space->next_region_id = 1u;
	return true;
}

bool memory_region_create(struct address_space* space, uintptr_t requested_base, const struct vmm_alloc_params* params,
                          struct memory_region_create_result* out_result) {
	struct memory_region* region;
	uintptr_t             reserved_base = 0;
	uintptr_t             base          = 0;
	size_t                align_pages;
	size_t                guard_pages;
	size_t                reserved_page_count;

	if (out_result) *out_result = (struct memory_region_create_result){0};
	if (!space || !params || !out_result || params->page_count == 0) return false;
	if (requested_base != 0 && (requested_base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (!address_space_is_initialized(space)) return false;
	if (!memory_region_params_allowed(space, params)) return false;

	align_pages = params->align_pages != 0 ? params->align_pages : VMM_MIN_ALIGN_PAGES;
	if ((align_pages & (align_pages - 1u)) != 0) return false;
	guard_pages = 0;
	if (params->kind == VMM_KIND_STACK) {
		guard_pages = params->guard_pages != 0 ? params->guard_pages : VMM_STACK_DEFAULT_GUARD_PAGES;
		if ((guard_pages % align_pages) != 0) return false;
	}
	else if (params->guard_pages != 0) return false;
	if (add_overflow_size(params->page_count, guard_pages, &reserved_page_count)) return false;
	if (reserved_page_count == 0) return false;

	if (requested_base == 0) {
		if (!address_space_reserve(space, reserved_page_count, align_pages, &reserved_base)) return false;
	}
	else {
		uint64_t guard_span;
		if (((requested_base / (uintptr_t)PMM_PAGE_SIZE) & (uintptr_t)(align_pages - 1u)) != 0) return false;
		if (mul_overflow_u64((uint64_t)guard_pages, PMM_PAGE_SIZE, &guard_span) ||
		    (uint64_t)requested_base < guard_span)
			return false;
		reserved_base = requested_base - (uintptr_t)guard_span;
		if (!address_space_reserve_at(space, reserved_base, reserved_page_count)) return false;
	}
	base = reserved_base + guard_pages * (uintptr_t)PMM_PAGE_SIZE;
	if (!range_contains(space, reserved_base, reserved_page_count) || !memory_region_ensure_capacity(space)) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		return false;
	}
	region = find_free_slot(space);
	if (!region) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		return false;
	}
	*region = (struct memory_region){
		.id                  = space->next_region_id++,
		.reserved_base       = reserved_base,
		.base                = base,
		.reserved_page_count = reserved_page_count,
		.page_count          = params->page_count,
		.prot                = params->prot,
		.kind                = params->kind,
		.guard_pages         = guard_pages,
		.map_flags           = params->map_flags,
		.owns_pages          = true,
		.used                = true,
	};
	backing_store_init(&region->backing, params->page_count);
	region_presence_init(&region->presence);
	space->region_count++;
	*out_result = (struct memory_region_create_result){
		.region = region,
		.base   = base,
	};
	return true;
}

bool memory_region_create_phys(struct address_space* space, uintptr_t requested_base, uintptr_t phys_base,
                               const struct vmm_alloc_params* params, struct memory_region** out_region,
                               uintptr_t* out_base) {
	struct memory_region* region;
	uintptr_t             reserved_base = 0;
	uintptr_t             base          = 0;
	size_t                align_pages;
	size_t                reserved_page_count;
	uint64_t              last_page_offset;
	uint64_t              last_page_base;

	if (out_region) *out_region = NULL;
	if (out_base) *out_base = 0;
	if (!space || !params || !out_region || params->page_count == 0) return false;
	if ((phys_base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (requested_base != 0 && (requested_base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (!address_space_is_initialized(space)) return false;
	if (!memory_region_params_allowed(space, params)) return false;
	if (params->kind != VMM_KIND_PHYSICAL) return false;
	if (params->guard_pages != 0) return false;
	if ((params->map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0) return false;
	if (mul_overflow_u64((uint64_t)(params->page_count - 1u), PMM_PAGE_SIZE, &last_page_offset) ||
	    add_overflow_u64((uint64_t)phys_base, last_page_offset, &last_page_base) ||
	    last_page_base > (uint64_t)UINTPTR_MAX - (PMM_PAGE_SIZE - 1u)) {
		return false;
	}

	align_pages = params->align_pages != 0 ? params->align_pages : VMM_MIN_ALIGN_PAGES;
	if ((align_pages & (align_pages - 1u)) != 0) return false;
	reserved_page_count = params->page_count;

	if (requested_base == 0) {
		if (!address_space_reserve(space, reserved_page_count, align_pages, &reserved_base)) return false;
	}
	else {
		if (((requested_base / (uintptr_t)PMM_PAGE_SIZE) & (uintptr_t)(align_pages - 1u)) != 0) return false;
		if (!address_space_reserve_at(space, requested_base, reserved_page_count)) return false;
		reserved_base = requested_base;
	}
	base = reserved_base;
	if (!range_contains(space, reserved_base, reserved_page_count) || !memory_region_ensure_capacity(space)) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		return false;
	}
	region = find_free_slot(space);
	if (!region) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		return false;
	}
	*region = (struct memory_region){
		.id                  = space->next_region_id++,
		.reserved_base       = reserved_base,
		.base                = base,
		.reserved_page_count = reserved_page_count,
		.page_count          = params->page_count,
		.prot                = params->prot,
		.kind                = params->kind,
		.guard_pages         = 0,
		.map_flags           = params->map_flags,
		.owns_pages          = false,
		.used                = true,
	};
	backing_store_init(&region->backing, params->page_count);
	region_presence_init(&region->presence);
	if (!backing_store_ensure(&region->backing)) {
		(void)address_space_release(space, reserved_base, reserved_page_count);
		memset(region, 0, sizeof(*region));
		return false;
	}
	for (size_t i = 0; i < params->page_count; i++) {
		backing_store_set_entry(&region->backing, i, phys_base + i * (uintptr_t)PMM_PAGE_SIZE);
	}
	space->region_count++;
	*out_region = region;
	*out_base   = base;
	return true;
}

bool memory_region_destroy(struct address_space* space, struct memory_region* region) {
	if (!space || !region || !region->used) return false;
	region_presence_release(&region->presence);
	if (region->owns_pages) {
		backing_store_release(&region->backing);
	}
	else {
		backing_store_release_metadata(&region->backing);
	}
	(void)address_space_release(space, region->reserved_base, region->reserved_page_count);
	memset(region, 0, sizeof(*region));
	space->region_count--;
	return true;
}

void memory_region_destroy_all(struct address_space* space) {
	if (!space) return;
	if (space->regions != NULL) {
		for (size_t i = 0; i < space->regions_capacity; i++) {
			if (!space->regions[i].used) continue;
			region_presence_release(&space->regions[i].presence);
			if (space->regions[i].owns_pages) {
				backing_store_release(&space->regions[i].backing);
			}
			else {
				backing_store_release_metadata(&space->regions[i].backing);
			}
			space->regions[i].used = false;
		}
		free_metadata(space->regions_phys, space->regions_page_count);
	}
	space->regions            = NULL;
	space->regions_phys       = 0;
	space->regions_page_count = 0;
	space->regions_capacity   = 0;
	space->region_count       = 0;
	space->next_region_id     = 1u;
}

struct memory_region* memory_region_at(struct address_space* space, size_t index) {
	if (!space || index >= space->regions_capacity) return NULL;
	return &space->regions[index];
}

size_t memory_region_capacity(const struct address_space* space) {
	return space != NULL ? space->regions_capacity : 0;
}

size_t memory_region_count(const struct address_space* space) {
	return space != NULL ? space->region_count : 0;
}

bool memory_region_is_used(const struct memory_region* region) {
	return region != NULL && region->used;
}

enum vmm_state memory_region_state(const struct memory_region* region) {
	if (!region || region_presence_mapped_count(&region->presence) == 0) return VMM_STATE_RESERVED;
	if (region_presence_mapped_count(&region->presence) == region->page_count) return VMM_STATE_MAPPED;
	return VMM_STATE_PARTIAL;
}

bool memory_region_stack_fault_is_valid(const struct memory_region* region, size_t page_index) {
	size_t mapped_count;
	size_t next_page;

	if (!region || region->kind != VMM_KIND_STACK) return true;
	if (page_index >= region->page_count) return false;
	mapped_count = region_presence_mapped_count(&region->presence);
	if (mapped_count >= region->page_count) return false;
	next_page = region->page_count - mapped_count - 1u;
	return page_index == next_page;
}

void memory_region_fill_info(const struct memory_region* region, struct vmm_info* out_info) {
	if (!region || !out_info) return;
	*out_info = (struct vmm_info){
		.id          = region->id,
		.base        = (void*)region->base,
		.page_count  = region->page_count,
		.prot        = region->prot,
		.kind        = region->kind,
		.guard_pages = region->guard_pages,
		.state       = memory_region_state(region),
		.first_phys  = backing_page_phys(backing_store_entry(&region->backing, 0)),
	};
}
